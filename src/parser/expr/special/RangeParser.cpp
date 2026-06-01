/**
 * @file RangeParser.cpp
 * @brief Parses inclusive and exclusive range expressions (`lo..hi`, `lo..<hi`).
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of range literals, which represent a sequence
 * of values from a lower bound to an upper bound. Ranges are used in:
 *   - `for` loops:      `for i in 0..10 { ... }`
 *   - `match` patterns: `case 1..10 => "light"`
 *   - Slice indices:    `nums[1..3]`
 * 
 * Grammar (from LUC_GRAMMAR.md):
 *   range_expr := expr ( '..' | '..<' ) expr
 * 
 *   - `..`   inclusive upper bound (e.g., 0..10 includes 10)
 *   - `..<`  exclusive upper bound (e.g., 0..<10 includes 0–9)
 * 
 * Examples:
 *   0..10    → inclusive range (0 through 10)
 *   1..<5    → exclusive range (1 through 4)
 *   start..end → generic range where start and end are expressions
 * 
 * @see ParserExpr.cpp for expression dispatch
 * @see RangeExprAST for AST representation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Range Expression
// ============================================================================

/**
 * @brief Parses a range expression: `lo .. hi` or `lo ..< hi`.
 *
 * Grammar:
 *   range_expr := expr ( '..' | '..<' ) expr
 *
 * @param lo The left‑hand side expression (already parsed).
 * @param allowStructLiteral Whether struct literals are allowed in the RHS.
 * @return ExprPtr – RangeExprAST on success, UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the range operator ('..' or '..<') – the `lo`
 *           expression has already been consumed by the caller.
 * On exit:  positioned after the `hi` expression.
 *
 * ─── Inclusive vs Exclusive ───────────────────────────────────────────────
 *   - `..`   : upper bound is inclusive (e.g., `0..10` includes 10)
 *   - `..<`  : upper bound is exclusive (e.g., `0..<10` includes 0–9)
 *
 * ─── Usage Contexts ───────────────────────────────────────────────────────
 *   | Context        | Example                          |
 *   |----------------|----------------------------------|
 *   | For loop       | `for i int in 0..10 { ... }`     |
 *   | Match pattern  | `case 1..10 => "light"`          |
 *   | Slice index    | `nums[1..3]`                     |
 *
 * ─── Semantic Constraints (Not Parser Responsibility) ─────────────────────
 *   - Both `lo` and `hi` must evaluate to the same numeric type.
 *   - In `for` loops, the range must be constant (compile‑time known).
 *   - In slice indices, the range must be within the array bounds.
 *
 * ─── Precedence ───────────────────────────────────────────────────────────
 *   The range operator has precedence PREC_ADD (level 10) – lower than
 *   multiplication but higher than comparison. This ensures `a * 2 .. b + 1`
 *   is parsed as `(a * 2) .. (b + 1)`.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing `hi` expression after range operator: reports error, returns
 *     UnknownExprAST (the caller may substitute a placeholder).
 *   - The `lo` is already parsed and may be used in error messages.
 *
 * ─── Example Parsing ──────────────────────────────────────────────────────
 *   Input:  `1 + 2 .. 3 * 4`
 *   Steps:  parseRangeExpr(parseExpr() for `1 + 2`) → lo = BinaryExpr(1+2)
 *           consumes `..`, then parsePrattExpr(PREC_ADD) → hi = BinaryExpr(3*4)
 *           returns RangeExprAST { lo = (1+2), hi = (3*4), isExclusive = false }
 */
ExprPtr Parser::parseRangeExpr(ExprPtr lo, bool allowStructLiteral) {
    SourceLocation loc = lo->loc;
    ts_.consume(TokenType::RANGE, "expected '..'");

    bool isExclusive = ts_.match(TokenType::LESS);
    ExprPtr hi = parsePrattExpr(PREC_ADD, allowStructLiteral);
    if (!hi) {
        errorAt(DiagCode::E2008, "expected upper bound after '..'");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<RangeExprAST>();
    node->loc = loc;
    node->lo = std::move(lo);
    node->hi = std::move(hi);
    node->isExclusive = isExclusive;
    return node;
}