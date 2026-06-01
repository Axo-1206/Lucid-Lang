/**
 * @file BinaryParser.cpp
 * @brief Parses infix binary operators, assignment, type checks, and null coalescing.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the infix operator handlers for the Pratt parser.
 * These functions are called from `parsePrattExpr()` when an infix operator
 * is encountered. Each function consumes the operator token, parses the
 * right operand (with appropriate precedence), and builds the corresponding
 * AST node.
 * 
 * Grammar (from LUC_GRAMMAR.md):
 *   assign_op       := '=' | '+=' | '-=' | '*=' | '/=' | '^=' | '%='
 *                    | '&&=' | '||=' | '~^=' | '<<=' | '>>='
 * 
 *   compare_op      := '==' | '!=' | '<' | '>' | '<=' | '>=' | '==='
 * 
 *   is_expr         := expr 'is' type
 * 
 *   null_coalesce   := expr '??' expr
 * 
 *   binary_op       := '+' | '-' | '*' | '/' | '^' | '%' | '&&' | '||' | '~^'
 *                    | '<<' | '>>' | 'and' | 'or'
 * 
 * ─── Precedence Levels (from highest to lowest) ──────────────────────────
 *   Level 12 : '^' (exponentiation, right‑associative)
 *   Level 11 : '*', '/', '%'
 *   Level 10 : '+', '-'
 *   Level 8  : '&&', '||', '~^', '<<', '>>' (bitwise)
 *   Level 7  : '==', '!=', '<', '>', '<=', '>=', 'is', '==='
 *   Level 6  : 'and'
 *   Level 5  : 'or'
 *   Level 4  : '??' (null coalesce)
 *   Level 1  : '=', '+=', '-=', etc. (assignment)
 * 
 * @see ParserExpr.cpp for Pratt parser core
 * @see ParserHelpers.cpp for parseType, parsePrattExpr helpers
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Assignment Operator
// ============================================================================

/**
 * @brief Parses an assignment expression (`lhs = rhs` or compound assignment).
 *
 * Grammar:
 *   assign_expr := lhs ( '=' | '+=' | '-=' | '*=' | '/=' | '^=' | '%='
 *                     | '&&=' | '||=' | '~^=' | '<<=' | '>>=' ) assign_expr
 *
 * @param lhs The left‑hand side expression (already parsed).
 * @param allowStructLiteral Whether struct literals are allowed in the RHS.
 * @return ExprPtr – AssignExprAST on success, or the original lhs on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the assignment operator token.
 * On exit:  positioned after the right‑hand side expression.
 *
 * ─── Examples ─────────────────────────────────────────────────────────────
 *   x = 5           → op = Assign
 *   x += 1          → op = AddAssign (desugars to `x = x + 1` in semantic pass)
 *   x &&= mask      → op = BitAndAssign
 *
 * ─── Precedence ───────────────────────────────────────────────────────────
 *   Assignment operators have the lowest precedence (PREC_ASSIGN = 1).
 *   The right operand is parsed with `prec - 1` to ensure right‑associativity.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing right operand: reports error, returns lhs (partial AST).
 *
 * ─── Semantic Notes ───────────────────────────────────────────────────────
 *   - The left‑hand side must be an lvalue (variable, field access, or index).
 *   - Compound operators desugar to `lhs = lhs op rhs` during semantic analysis.
 *   - Assignment to a `const` variable is a semantic error.
 */
ExprPtr Parser::parseInfixAssign(ExprPtr lhs, bool allowStructLiteral) {
    TokenType opTok = ts_.advance().type;
    AssignOp op = tokenToAssignOp(opTok);
    
    ExprPtr rhs = parsePrattExpr(PREC_ASSIGN - 1, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected expression after assignment operator");
        return lhs;
    }

    auto node = arena_.make<AssignExprAST>();
    node->loc = lhs->loc;
    node->op = op;
    node->lhs = std::move(lhs);
    node->rhs = std::move(rhs);
    return node;
}

// ============================================================================
// Type Check Operator (`is`)
// ============================================================================

/**
 * @brief Parses a type check expression (`lhs is Type`).
 *
 * Grammar:
 *   is_expr := expr 'is' type
 *
 * @param lhs The left‑hand side expression (already parsed).
 * @return ExprPtr – IsExprAST on success, or the original lhs on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the `is` keyword.
 * On exit:  positioned after the type.
 *
 * ─── Example ──────────────────────────────────────────────────────────────
 *   x is int                    → checks if x has type int
 *   shape is Circle             → checks if shape is a Circle
 *   stage is ShaderStage.Fragment
 *
 * ─── Type Narrowing ───────────────────────────────────────────────────────
 *   When used in a standalone `if` condition, the semantic pass narrows the
 *   type of the left‑hand side inside the then‑branch:
 *     if x is int { ... }   → x is treated as int inside the block.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing type after `is`: reports error, returns lhs.
 */
ExprPtr Parser::parseInfixIs(ExprPtr lhs) {
    ts_.advance();  // consume 'is'
    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E2005, "expected type after 'is'");
        return lhs;
    }

    auto node = arena_.make<IsExprAST>();
    node->loc = lhs->loc;
    node->expr = std::move(lhs);
    node->checkType = std::move(checkType);
    return node;
}

// ============================================================================
// Null Coalescing Operator (`??`)
// ============================================================================

/**
 * @brief Parses a null coalescing expression (`lhs ?? fallback`).
 *
 * Grammar:
 *   null_coalesce := expr '??' expr
 *
 * The `??` operator provides a fallback value when the left‑hand side is
 * either `nil` or an unresolved `T!E` (result type). After `??`, the result
 * is always plain `T`.
 *
 * @param lhs The left‑hand side expression (already parsed).
 * @param allowStructLiteral Whether struct literals are allowed in the RHS.
 * @return ExprPtr – NullCoalesceExprAST on success, or the original lhs on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the `??` token.
 * On exit:  positioned after the fallback expression.
 *
 * ─── Example ──────────────────────────────────────────────────────────────
 *   value ?? 0                 → if value is nil, result is 0
 *   riskyOp() ?? defaultValue   → if riskyOp() returns T!E (unresolved), result is defaultValue
 *
 * ─── Precedence ───────────────────────────────────────────────────────────
 *   Right‑associative, precedence PREC_NULLCOAL = 4.
 *   The right operand is parsed with `prec - 1` for right‑associativity.
 *
 * ─── Comparison with `resolve` ────────────────────────────────────────────
 *   - `??` discards the error and provides a default.
 *   - `resolve` allows inspecting the error and full control flow.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing fallback expression: reports error, returns lhs.
 */
ExprPtr Parser::parseInfixNullCoalesce(ExprPtr lhs, bool allowStructLiteral) {
    ts_.advance();  // consume '??'
    
    ExprPtr fallback = parsePrattExpr(PREC_NULLCOAL - 1, allowStructLiteral);
    if (!fallback) {
        errorAt(DiagCode::E2008, "expected expression after '\?\?'");
        return lhs;
    }

    auto node = arena_.make<NullCoalesceExprAST>();
    node->loc = lhs->loc;
    node->value = std::move(lhs);
    node->fallback = std::move(fallback);
    return node;
}

// ============================================================================
// Binary Operators (Arithmetic, Comparison, Logical, Bitwise)
// ============================================================================

/**
 * @brief Parses a binary expression (infix operators except assignment and `is`).
 *
 * This handles arithmetic (`+`, `-`, `*`, `/`, `^`, `%`), comparison
 * (`==`, `!=`, `<`, `>`, `<=`, `>=`, `===`), logical (`and`, `or`), and
 * bitwise (`&&`, `||`, `~^`, `<<`, `>>`) operators.
 *
 * @param lhs The left‑hand side expression (already parsed).
 * @param opTok The token type of the operator (e.g., TokenType::PLUS).
 * @param prec The precedence level of the operator.
 * @param allowStructLiteral Whether struct literals are allowed in the RHS.
 * @return ExprPtr – BinaryExprAST on success, or the original lhs on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the operator token.
 * On exit:  positioned after the right‑hand side expression.
 *
 * ─── Examples ─────────────────────────────────────────────────────────────
 *   a + b           → op = Add
 *   x == y          → op = Eq
 *   p and q         → op = And (short‑circuit)
 *   a && b          → op = BitAnd (bitwise AND, integer types only)
 *   ~^              → op = BitXor
 *
 * ─── Precedence Handling ──────────────────────────────────────────────────
 *   - For most operators, the right operand is parsed with the same precedence.
 *   - For right‑associative operators (`^`), uses `prec - 1`.
 *
 * ─── Chained Comparison Detection ─────────────────────────────────────────
 *   Luc does NOT allow chained comparisons like `a < b < c`. The grammar
 *   requires explicit `and`: `a < b and b < c`. This function detects and
 *   reports an error when two comparison operators appear consecutively.
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing right operand: reports error, returns lhs.
 *   - Chained comparison detected: reports error, still builds the AST
 *     (the error is non‑fatal, but the semantic pass may reject it).
 *
 * ─── Valid Types ──────────────────────────────────────────────────────────
 *   - Arithmetic: numeric types (int, float, double, decimal)
 *   - Comparison: primitives, enums, nullable types; structs require `:equals()`
 *   - Logical (`and`, `or`): `bool` or nullable types
 *   - Bitwise: integer types only
 */
ExprPtr Parser::parseInfixBinary(ExprPtr lhs, TokenType opTok, int prec, bool allowStructLiteral) {
    ts_.advance();  // consume operator
    
    int nextPrec = (opTok == TokenType::POW) ? prec - 1 : prec;
    ExprPtr rhs = parsePrattExpr(nextPrec, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E2008, "expected right-hand side of binary expression");
        return lhs;
    }

    // Chained comparison detection (e.g., `a < b < c`)
    auto isComparisonOp = [](TokenType tt) {
        switch (tt) {
            case TokenType::EQUAL_EQUAL:
            case TokenType::EQUAL_EQUAL_EQUAL:
            case TokenType::NOT_EQUAL:
            case TokenType::LESS:
            case TokenType::GREATER:
            case TokenType::LESS_EQUAL:
            case TokenType::GREATER_EQUAL:
            case TokenType::IS:
                return true;
            default:
                return false;
        }
    };

    if (isComparisonOp(opTok) && isComparisonOp(ts_.peekType())) {
        errorAt(DiagCode::E2002, "chained comparisons not allowed; use 'and' explicitly");
    }

    auto node = arena_.make<BinaryExprAST>();
    node->loc = lhs->loc;
    node->op = tokenToBinaryOp(opTok);
    node->left = std::move(lhs);
    node->right = std::move(rhs);
    return node;
}