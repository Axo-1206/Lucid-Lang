/**
 * @file TypeConvParser.cpp
 * @brief Parses safe explicit type casts: `type(expr)`.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of safe explicit type conversion expressions.
 * 
 * Grammar (from LUC_GRAMMAR.md):
 *   Safe cast: type_name '(' expr ')'
 * 
 * Examples:
 *   float(x)      – convert integer to float
 *   string(42)    – convert integer to string
 *   int(myEnum)   – convert enum to its underlying integer type
 * 
 * ─── What is NOT Allowed ─────────────────────────────────────────────────
 *   - Casting to reference types (`&T(expr)`) – use borrowing or `#toRef`.
 *   - Casting to pointer types (`*T(expr)`) – use `#toPtr` intrinsic.
 *   - Unsafe bit reinterpretation – use `#bitcast` intrinsic.
 * 
 * ─── Why No Unsafe Casts? ────────────────────────────────────────────────
 *   Raw pointers (`*T`) are "sealed conduits" – they cannot be constructed
 *   arbitrarily. Use `#toPtr` to get a raw pointer from a reference, or
 *   `#toRef` to go the other way. Unsafe bit reinterpretation is done via
 *   `#bitcast`, which is explicit and requires `--unsafe` flag.
 * 
 * @see ParserExpr.cpp for expression dispatch
 * @see TypeConvExprAST for AST representation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Safe Type Conversion (Cast)
// ============================================================================

/**
 * @brief Parses a safe explicit type cast: `type(expr)`.
 *
 * Grammar:
 *   Safe cast: type_name '(' expr ')'
 *
 * @param targetType The parsed target type (must be a primitive, enum, or string).
 * @return ExprPtr – TypeConvExprAST on success, UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned after the type name, current token is '('.
 * On exit:  positioned after the closing ')'.
 *
 * ─── Example ──────────────────────────────────────────────────────────────
 *   float(x)   → targetType = float, expr = x
 *
 * ─── Supported Conversions (Semantic Pass) ────────────────────────────────
 *   - Primitive widening: int16 → int32, float → double
 *   - Enum to underlying integer type
 *   - Integer to string
 *
 * ─── Forbidden Conversions (Semantic Pass) ────────────────────────────────
 *   - Reference types (`&T(expr)`)
 *   - Pointer types (`*T(expr)`)
 *   - Any unsafe bit reinterpretation (use `#bitcast` instead)
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing '(' after type name: consume() reports error, returns UnknownExprAST.
 *   - Missing expression inside parentheses: reports error, returns UnknownExprAST.
 *   - Missing ')': consume() reports error.
 */
ExprPtr Parser::parseTypeConvExpr(TypePtr targetType) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LPAREN, "expected '(' for explicit type cast");

    ExprPtr expr = parseExpr();
    if (!expr) {
        errorAt(DiagCode::E1008, "expected expression inside explicit type cast");
        return arena_.make<UnknownExprAST>();
    }

    ts_.consume(TokenType::RPAREN, "expected ')' to close explicit type cast");

    auto node = arena_.make<TypeConvExprAST>(std::move(targetType), std::move(expr), false);
    node->loc = loc;
    return node;
}