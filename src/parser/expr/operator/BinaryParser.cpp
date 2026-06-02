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

ExprPtr Parser::parseInfixAssign(ExprPtr lhs, bool allowStructLiteral) {
    TokenType opTok = ts_.advance().type;
    AssignOp op = tokenToAssignOp(opTok);
    
    ExprPtr rhs = parsePrattExpr(PREC_ASSIGN - 1, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E1008, "expected expression after assignment operator");
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

ExprPtr Parser::parseInfixIs(ExprPtr lhs) {
    ts_.advance();  // consume 'is'
    TypePtr checkType = parseType();
    if (!checkType) {
        errorAt(DiagCode::E1005, "expected type after 'is'");
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

ExprPtr Parser::parseInfixNullCoalesce(ExprPtr lhs, bool allowStructLiteral) {
    ts_.advance();  // consume '??'
    
    ExprPtr fallback = parsePrattExpr(PREC_NULLCOAL - 1, allowStructLiteral);
    if (!fallback) {
        errorAt(DiagCode::E1008, "expected expression after '\?\?'");
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

ExprPtr Parser::parseInfixBinary(ExprPtr lhs, TokenType opTok, int prec, bool allowStructLiteral) {
    ts_.advance();  // consume operator
    
    int nextPrec = (opTok == TokenType::POW) ? prec - 1 : prec;
    ExprPtr rhs = parsePrattExpr(nextPrec, allowStructLiteral);
    if (!rhs) {
        errorAt(DiagCode::E1008, "expected right-hand side of binary expression");
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
        errorAt(DiagCode::E1002, "chained comparisons not allowed; use 'and' explicitly");
    }

    auto node = arena_.make<BinaryExprAST>();
    node->loc = lhs->loc;
    node->op = tokenToBinaryOp(opTok);
    node->left = std::move(lhs);
    node->right = std::move(rhs);
    return node;
}