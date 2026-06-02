/**
 * @file IndexParser.cpp
 * @brief Parses array element access and slice expressions.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements parsing of array indexing and slicing operations.
 * The `parseIndexExpr()` function is called from `parsePostfixExpr()` when a
 * `[` token is encountered after an array‑typed expression.
 * 
 * Grammar (from LUC_GRAMMAR.md):
 *   postfix_op := '[' expr ']'                    -- element access
 *               | '[' expr '..' expr ']'          -- inclusive slice
 *               | '[' expr '..<' expr ']'         -- exclusive slice
 * 
 * Examples:
 *   nums[2]                    → element access (index 2)
 *   nums[1..3]                 → inclusive slice (elements 1, 2, 3)
 *   nums[1..<3]                → exclusive slice (elements 1, 2)
 *   matrix[row][col]           → nested index (handled by repeated calls)
 *   values[i] = 42             → indexed assignment (lvalue context)
 * 
 * @see ParserExpr.cpp for parsePostfixExpr integration
 * @see IndexExprAST in ExprAST.hpp for AST representation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Index Expression Parser
// ============================================================================

/**
 * @brief Parses an array element access or slice expression.
 *
 * Grammar:
 *   index_expr := '[' expr [ ( '..' | '..<' ) expr ] ']'
 *
 * This function consumes the bracketed expression(s) and determines whether
 * the operation is element access (single index) or slice (start and end).
 *
 * @param target The array‑typed expression being indexed (already parsed).
 * @return ExprPtr – IndexExprAST on success, or UnknownExprAST on error.
 *
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '['
 * On exit:  positioned after the closing ']'
 *
 * ─── Element Access (Single Index) ────────────────────────────────────────
 *   Format: `[ index ]`
 *   - Parses a single expression as the index.
 *   - Sets `kind = IndexKind::Element`
 *   - `sliceEnd` remains nullptr.
 *
 * ─── Slice Access (Range) ─────────────────────────────────────────────────
 *   Format: `[ start .. end ]` or `[ start ..< end ]`
 *   - Parses start expression, then consumes '..' (or '..<')
 *   - `isExclusive = true` for '..<', false for '..'
 *   - Parses end expression as the upper bound
 *   - Sets `kind = IndexKind::Slice`
 *   - Both `index` and `sliceEnd` are populated.
 *
 * ─── Array Types and Bounds ───────────────────────────────────────────────
 *   - Fixed arrays (`[N, T]`): bounds checked at compile time (if constant)
 *   - Dynamic arrays (`[*, T]`): bounds checked at runtime (returns nil on OOB)
 *   - Slices (`[_, T]`): bounds checked at runtime (panics on OOB)
 *
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing '[': handled by caller (consume() reports error before entering)
 *   - Missing start expression: reports error, returns UnknownExprAST
 *   - Missing end expression after '..': reports error, returns UnknownExprAST
 *   - Missing closing ']': consume() reports error
 *
 * ─── Semantic Notes (Not Parser Responsibility) ───────────────────────────
 *   - Target must be an array type (fixed, dynamic, or slice)
 *   - Index expression must be integer type (int, uint, etc.)
 *   - Slice bounds must be within array dimensions
 *   - Result type: element T for element access, slice `[_, T]` for slice access
 */
ExprPtr Parser::parseIndexExpr(ExprPtr target) {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    ExprPtr startExpr = parseExpr();
    if (!startExpr) {
        errorAt(DiagCode::E1008, "expected index expression");
        return arena_.make<UnknownExprAST>();
    }

    auto node = arena_.make<IndexExprAST>();
    node->loc = loc;
    node->target = std::move(target);

    // Check for slice syntax: start .. end or start ..< end
    if (ts_.check(TokenType::RANGE)) {
        ts_.advance();  // consume '..'
        bool isExclusive = ts_.match(TokenType::LESS);
        
        ExprPtr endExpr = parseExpr();
        if (!endExpr) {
            errorAt(DiagCode::E1008, "expected end of slice range after '..'");
            return arena_.make<UnknownExprAST>();
        }
        
        node->index = std::move(startExpr);
        node->sliceEnd = std::move(endExpr);
        node->kind = IndexKind::Slice;
        node->isExclusive = isExclusive;
    } else {
        // Simple element access: [ index ]
        node->index = std::move(startExpr);
        node->kind = IndexKind::Element;
    }

    ts_.consume(TokenType::RBRACKET, "expected ']' to close index expression");
    return node;
}