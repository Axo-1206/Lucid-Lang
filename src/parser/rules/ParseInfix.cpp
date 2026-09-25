/**
 * @file ParseInfix.cpp
 * @brief The infix dispatchers and precedence helpers for the Pratt loop.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseInfixAssign         a = b, a += b, and the other compound forms
 *   - parseInfixNullCoalesce   a ?? b
 *   - parseInfixBinary         a + b, a == b, a and b, and the rest
 *   - infixPrec                binding power of an infix operator token
 *   - tokenToBinaryOp          token → BinaryOp enum
 *   - tokenToAssignOp          token → AssignOp enum
 *
 * ─── Design: the Pratt loop and this file ─────────────────────────────────
 * The Pratt loop (`parsePrattExpr` in ParseExpr.cpp) has the following
 * shape:
 *
 *     ExprAST* parsePrattExpr(TokenStream& stream, ParserContext& ctx,
 *                             int minPrec) {
 *         ExprAST* lhs = parsePrefixExpr(stream, ctx);
 *         if (!lhs) return nullptr;
 *
 *         while (!stream.isAtEnd()) {
 *             TokenType current = stream.peekType();
 *
 *             // Postfix operators (calls, index, field access) bind
 *             // tighter than any infix; parsePostfixExpr handles them.
 *             if (is_postfix_start(current)) {
 *                 lhs = parsePostfixExpr(stream, ctx, lhs);
 *                 continue;
 *             }
 *
 *             // Assignment is right-associative and looser than any
 *             // binary operator. Checked before the precedence cutoff.
 *             if (is_assignment_op(current)) {
 *                 stream.consume();
 *                 lhs = parseInfixAssign(stream, ctx, lhs, current);
 *                 continue;
 *             }
 *
 *             // Null-coalesce is right-associative and looser than any
 *             // binary but tighter than pipeline.
 *             if (current == TokenType::QUESTION_QUESTION) {
 *                 stream.consume();
 *                 lhs = parseInfixNullCoalesce(stream, ctx, lhs);
 *                 continue;
 *             }
 *
 *             // Standard binary operators use precedence climbing.
 *             int prec = infixPrec(current);
 *             if (prec < minPrec) break;
 *             if (prec >= 0) {
 *                 stream.consume();
 *                 lhs = parseInfixBinary(stream, ctx, lhs, current, prec);
 *                 continue;
 *             }
 *             break;
 *         }
 *
 *         return lhs;
 *     }
 *
 * The three `parseInfix*` functions in this file are called by that loop.
 * Each consumes the operator token, parses the right-hand side at the
 * correct precedence, and produces the AST node.
 *
 * ─── Design: precedence lives in one place ────────────────────────────────
 * `infixPrec` is the single source of truth for operator precedence. The
 * grammar's precedence table is reflected here and nowhere else. If the
 * table changes, this function changes; nothing else does.
 *
 * ─── Design: assignment and null-coalesce are not in infixPrec ────────────
 * Assignment and `??` are handled specially by the Pratt loop (checked
 * before the precedence cutoff), because they have associativity that
 * differs from the binary operators. `infixPrec` returns a negative
 * sentinel for these tokens; the loop's earlier checks catch them before
 * the cutoff.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// infixPrec — binding power of an infix operator token
// =============================================================================

/// @brief The binding power of an infix operator token.
///
/// The value is the operator's precedence level from the grammar's
/// precedence table. Higher values bind tighter. The Pratt loop uses the
/// value to decide whether to keep consuming infix operators or return
/// to the caller.
///
/// Return values:
///   - A non-negative integer: a real infix operator with this precedence.
///   - `-1`: the pipeline operator `|>`, which is handled specially by
///     `parsePostfixExpr`, not by the Pratt loop's precedence climbing.
///     (The pipeline is a postfix form in the grammar; it is listed here
///     so that a call site that does reach `infixPrec` for `|>` gets a
///     negative result and stops the loop.)
///   - `-2`: not an infix operator. Assignment, `??`, and every non-
///     operator token return this.
///
/// Precedence levels (from the grammar):
///
///     Level  Operator                                              Assoc
///     -----  ----------------------------------------------------  -----
///     6      `*` `/` `%` `**`                                      left
///     5      `+` `-`                                               left
///     4      `..` `..<`                                            left
///     3      `==` `!=` `<` `<=` `>` `>=`                           left
///     2      `and`                                                 left
///     1      `or`                                                  left
///     0      `??`                                                  left (special)
///     -1     `|>`                                                  left (special)
///
/// `??` is listed at precedence 0 in the grammar's table, but the Pratt
/// loop handles it before the cutoff; `infixPrec` returns `-2` for it so
/// the loop's standard path is never reached for `??`. The same is true
/// for assignment operators.
///
/// This function is the parser's single source of truth for operator
/// precedence. Any change to the grammar's precedence table changes this
/// function and nothing else.
int infixPrec(TokenType type) {
    switch (type) {
        // ─── Multiplicative ─────────────────────────────────────────────
        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::MOD:
        case TokenType::POW:
            return 6;

        // ─── Additive ───────────────────────────────────────────────────
        case TokenType::PLUS:
        case TokenType::MINUS:
            return 5;

        // ─── Range ──────────────────────────────────────────────────────
        case TokenType::RANGE:
        case TokenType::RANGE_EXCLUSIVE:
            return 4;

        // ─── Comparison ─────────────────────────────────────────────────
        case TokenType::EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL:
            return 3;

        // ─── Logical `and` ──────────────────────────────────────────────
        //
        // `and` and `or` are not keywords in the current grammar; they
        // are identifiers whose meaning is resolved through the DEF
        // table. But the grammar's operator set lists them as operators
        // with precedence, and the Pratt loop treats them as infix. The
        // lexer emits them as IDENTIFIER; the parser's Pratt loop checks
        // the identifier's value to decide whether it is an operator.
        //
        // This branch is not reachable through the token type alone,
        // because `and` and `or` are not distinct token types. See the
        // note in `parsePrattExpr` (ParseExpr.cpp) about how `and`/`or`
        // are dispatched. If the design later promotes `and`/`or` to
        // keywords (KW_AND, KW_OR), this branch becomes live.
        // For now, it is a documented placeholder and the parser handles
        // `and`/`or` in the IDENTIFIER dispatch.

        // ─── Not an operator for the precedence loop ────────────────────
        //
        // Assignment, `??`, and pipeline are handled specially by the
        // Pratt loop before `infixPrec` is consulted. They return `-2`
        // here so a direct query gives a consistent answer.
        case TokenType::ASSIGN:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::MUL_ASSIGN:
        case TokenType::DIV_ASSIGN:
        case TokenType::MOD_ASSIGN:
        case TokenType::POW_ASSIGN:
        case TokenType::BIT_AND_ASSIGN:
        case TokenType::BIT_OR_ASSIGN:
        case TokenType::BIT_XOR_ASSIGN:
        case TokenType::SHL_ASSIGN:
        case TokenType::SHR_ASSIGN:
        case TokenType::QUESTION_QUESTION:
        case TokenType::PIPELINE:
        default:
            return -2;
    }
}

// =============================================================================
// tokenToBinaryOp — token → BinaryOp
// =============================================================================

/// @brief Map an operator token to its `BinaryOp` enum value.
///
/// Called by `parseInfixBinary` once it has decided a token is a binary
/// operator. The mapping is one-to-one for every operator token that
/// `infixPrec` returns a non-negative value for.
///
/// The `default` case returns `BinaryOp::Add` rather than throwing or
/// asserting. This case is unreachable when `parseInfixBinary` is called
/// correctly (the caller has already checked that the token is a binary
/// operator). If a future change adds a new binary operator token and
/// forgets to update this function, the mis-mapped token produces an
/// `Add` node instead of a crash. The AST is wrong, but the compiler
/// finishes; a Sema pass or a test catches it. This is preferable to
/// aborting mid-parse.
BinaryOp tokenToBinaryOp(TokenType type) {
    switch (type) {
        // Arithmetic
        case TokenType::PLUS:   return BinaryOp::Add;
        case TokenType::MINUS:  return BinaryOp::Sub;
        case TokenType::MUL:    return BinaryOp::Mul;
        case TokenType::DIV:    return BinaryOp::Div;
        case TokenType::POW:    return BinaryOp::Pow;
        case TokenType::MOD:    return BinaryOp::Mod;

        // Comparison
        case TokenType::EQUAL_EQUAL:   return BinaryOp::Eq;
        case TokenType::NOT_EQUAL:     return BinaryOp::Ne;
        case TokenType::LESS:          return BinaryOp::Lt;
        case TokenType::LESS_EQUAL:    return BinaryOp::Le;
        case TokenType::GREATER:       return BinaryOp::Gt;
        case TokenType::GREATER_EQUAL: return BinaryOp::Ge;

        // Bitwise
        case TokenType::BIT_AND: return BinaryOp::BitAnd;
        case TokenType::BIT_OR:  return BinaryOp::BitOr;
        case TokenType::BIT_XOR: return BinaryOp::BitXor;
        case TokenType::SHL:     return BinaryOp::Shl;
        case TokenType::SHR:     return BinaryOp::Shr;

        // `and`/`or` are not distinct tokens in the current grammar;
        // they are identifiers. The IdentifierExprAST dispatch in
        // parsePrattExpr handles them directly.
        //
        // `??` and assignment are handled by parseInfixNullCoalesce and
        // parseInfixAssign, not by parseInfixBinary.

        default:
            return BinaryOp::Add;
    }
}

// =============================================================================
// tokenToAssignOp — token → AssignOp
// =============================================================================

/// @brief Map an assignment token to its `AssignOp` enum value.
///
/// Called by `parseInfixAssign` once it has decided a token is an
/// assignment operator. The mapping is one-to-one for every assignment
/// token.
///
/// The `default` case returns `AssignOp::Assign` for the same defensive
/// reason as `tokenToBinaryOp`'s default.
AssignOp tokenToAssignOp(TokenType type) {
    switch (type) {
        case TokenType::ASSIGN:         return AssignOp::Assign;
        case TokenType::PLUS_ASSIGN:    return AssignOp::AddAssign;
        case TokenType::MINUS_ASSIGN:   return AssignOp::SubAssign;
        case TokenType::MUL_ASSIGN:     return AssignOp::MulAssign;
        case TokenType::DIV_ASSIGN:     return AssignOp::DivAssign;
        case TokenType::MOD_ASSIGN:     return AssignOp::ModAssign;
        case TokenType::POW_ASSIGN:     return AssignOp::PowAssign;
        case TokenType::BIT_AND_ASSIGN: return AssignOp::BitAndAssign;
        case TokenType::BIT_OR_ASSIGN:  return AssignOp::BitOrAssign;
        case TokenType::BIT_XOR_ASSIGN: return AssignOp::BitXorAssign;
        case TokenType::SHL_ASSIGN:     return AssignOp::ShlAssign;
        case TokenType::SHR_ASSIGN:     return AssignOp::ShrAssign;
        default:
            return AssignOp::Assign;
    }
}

// =============================================================================
// parseInfixAssign — assignment and compound assignment
// =============================================================================

/// @brief Parse the right-hand side of an assignment.
///
/// The caller (the Pratt loop) has already consumed the operator token.
/// This function parses the RHS at the assignment's precedence and
/// produces the `AssignExprAST`.
///
/// ─── Right-associativity ──────────────────────────────────────────────────
/// Assignment is right-associative: `a = b = c` parses as `a = (b = c)`.
/// The Pratt loop checks for assignment before the precedence cutoff, so
/// this function's recursive call to `parsePrattExpr` uses the
/// assignment's own precedence level, which is what makes the recursion
/// right-associative.
///
/// ─── Compound assignment desugars to `lhs = lhs op rhs` ───────────────────
/// A compound operator like `+=` produces an `AssignExprAST` whose `op`
/// is `AssignOp::AddAssign`, not an `AssignOp` for the compound directly.
/// Sema desugars the compound to a plain assign plus a binary operation.
/// The parser does not desugar; it produces the compound node with its
/// operator tag, and Sema does the desugaring.
///
/// `opTok` is the operator token the caller consumed; it is used only to
/// determine the `AssignOp` tag. The AST node does not store the token
/// itself.
ExprAST* parseInfixAssign(TokenStream& stream,
                          ParserContext& ctx,
                          ExprAST* lhs,
                          TokenType opTok) {
    const SourceLocation loc = stream.currentLoc();

    // Defensive: the caller should have passed a non-null lhs. If it
    // didn't, report and bail rather than crashing on the dereference
    // below.
    if (!lhs) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                           "expected left-hand side of assignment");
        return nullptr;
    }

    // ─── Parse the right-hand side ────────────────────────────────────────
    //
    // The RHS is parsed at the assignment's own precedence. Because the
    // Pratt loop checks for assignment before the precedence cutoff, the
    // recursion into parsePrattExpr with the assignment's precedence
    // produces right-associativity: a second `=` in the RHS is consumed
    // by the inner call, not by the outer.
    //
    // The assignment precedence is not a value in `infixPrec` (which
    // returns -2 for all assignment tokens). We use -1 as the sentinel
    // for "assignment precedence" — it's lower than any binary operator
    // and higher than the pipeline, which is what the grammar's table
    // says. If `infixPrec` later changes to include assignment, this
    // constant should be updated.
    constexpr int kAssignmentPrec = -1;
    ExprAST* rhs = parsePrattExpr(stream, ctx, kAssignmentPrec);

    if (!rhs) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected right-hand side of assignment");
        return nullptr;
    }

    // ─── Build the AssignExprAST ──────────────────────────────────────────
    const AssignOp op = tokenToAssignOp(opTok);
    auto* assign = ctx.arena().make<AssignExprAST>(op);
    assign->loc = loc;
    assign->lhs = lhs;
    assign->rhs = rhs;
    return assign;
}

// =============================================================================
// parseInfixNullCoalesce — the `??` operator
// =============================================================================

/// @brief Parse the right-hand side of a null-coalesce operator.
///
/// The caller (the Pratt loop) has already consumed the `??` token. This
/// function parses the RHS at the null-coalesce's precedence and produces
/// the `NullCoalesceExprAST`.
///
/// ─── Right-associativity ──────────────────────────────────────────────────
/// `??` is right-associative: `a ?? b ?? c` parses as `a ?? (b ?? c)`.
/// Like assignment, `??` is checked before the precedence cutoff, and
/// the recursive call uses the same precedence level, producing
/// right-associativity.
///
/// ─── Precedence ───────────────────────────────────────────────────────────
/// `??` is looser than `or` and tighter than `|>`. Its position in the
/// grammar's table is level 0 (between `or` at 1 and `|>` at -1). The
/// parser uses precedence `0` for the recursive call.
///
/// ─── Result type ──────────────────────────────────────────────────────────
/// The `??` operator always resolves the sentinel. The result type is the
/// plain type `T`, never `T?`, `T!`, or `T?!`. Sema computes the result
/// type from the LHS's inner type.
ExprAST* parseInfixNullCoalesce(TokenStream& stream,
                                ParserContext& ctx,
                                ExprAST* lhs) {
    const SourceLocation loc = stream.currentLoc();

    if (!lhs) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                           "expected left-hand side of '?\?'");
        return nullptr;
    }

    // ─── Parse the right-hand side ────────────────────────────────────────
    //
    // Right-associative: the RHS is parsed at `??`'s own precedence.
    constexpr int kNullCoalescePrec = 0;
    ExprAST* rhs = parsePrattExpr(stream, ctx, kNullCoalescePrec);

    if (!rhs) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected right-hand side of '?\?'");
        return nullptr;
    }

    // ─── Build the NullCoalesceExprAST ────────────────────────────────────
    auto* coalesce = ctx.arena().make<NullCoalesceExprAST>(lhs, rhs);
    coalesce->loc = loc;
    return coalesce;
}

// =============================================================================
// parseInfixBinary — binary operators
// =============================================================================

/// @brief Parse the right-hand side of a binary operator.
///
/// The caller (the Pratt loop) has already consumed the operator token
/// and computed its precedence (`prec`). This function parses the RHS at
/// the correct precedence and produces either a `BinaryExprAST` (for the
/// standard operators) or a `RangeExprAST` (for `..` and `..<`).
///
/// ─── Left-associativity ───────────────────────────────────────────────────
/// Binary operators are left-associative: `a - b - c` parses as
/// `(a - b) - c`. The Pratt loop consumes the first `-`, calls this
/// function with precedence `5` (additive), and the recursive call to
/// `parsePrattExpr` uses `prec + 1`. The `+ 1` is what enforces
/// left-associativity: the inner call refuses to consume an operator at
/// the same precedence level, so the inner call returns to the outer
/// loop, which then consumes the second `-`.
///
/// ─── Range exception ──────────────────────────────────────────────────────
/// The range operators `..` and `..<` use `prec` (not `prec + 1`) for the
/// RHS. This makes them parse their bounds tightly: `0..n - 1` parses as
/// `0..(n - 1)`, not `(0..n) - 1`. The grammar's precedence table puts
/// `..` at level 4 (tighter than `+`/`-` at 5), which means the range
/// binds its bounds more loosely than the arithmetic inside them — the
/// RHS is parsed at the same level as the range itself, so any
/// higher-precedence operator in the RHS is consumed by the RHS parse.
///
/// ─── `and`/`or` ───────────────────────────────────────────────────────────
/// The logical operators `and` and `or` are not distinct token types in
/// the current grammar; they are identifiers. The Pratt loop's
/// `IdentifierExprAST` dispatch checks the identifier's value to decide
/// whether it is `and` or `or` and, if so, calls this function with the
/// appropriate `prec`. The `tokenToBinaryOp` mapping below does not have
/// cases for them because they are not tokens.
///
/// ─── `prec` argument ──────────────────────────────────────────────────────
/// `prec` is the precedence of the operator, as computed by the caller
/// from `infixPrec`. Passing it in (rather than recomputing it here)
/// avoids a second lookup and makes the left/right associativity decision
/// local to this function.
ExprAST* parseInfixBinary(TokenStream& stream,
                          ParserContext& ctx,
                          ExprAST* lhs,
                          TokenType opTok,
                          int prec) {
    const SourceLocation loc = stream.currentLoc();

    if (!lhs) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                           "expected left-hand side of binary operator");
        return nullptr;
    }

    // ─── Range operators ──────────────────────────────────────────────────
    //
    // `..` and `..<` are parsed into RangeExprAST, not BinaryExprAST. They
    // use `prec` (not `prec + 1`) for the RHS, which makes them bind their
    // bounds tightly.
    const bool isRangeOp = (opTok == TokenType::RANGE ||
                            opTok == TokenType::RANGE_EXCLUSIVE);

    // ─── Parse the right-hand side ────────────────────────────────────────
    const int rhsPrec = isRangeOp ? prec : prec + 1;
    ExprAST* rhs = parsePrattExpr(stream, ctx, rhsPrec);

    if (!rhs) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected right-hand side of '",
                           stream.peekValue(), "'");
        return nullptr;
    }

    // ─── Build the node ───────────────────────────────────────────────────
    if (isRangeOp) {
        const bool isExclusive = (opTok == TokenType::RANGE_EXCLUSIVE);
        auto* range = ctx.arena().make<RangeExprAST>(isExclusive);
        range->loc = loc;
        range->lo = lhs;
        range->hi = rhs;
        return range;
    }

    const BinaryOp op = tokenToBinaryOp(opTok);
    auto* binary = ctx.arena().make<BinaryExprAST>(op);
    binary->loc = loc;
    binary->left = lhs;
    binary->right = rhs;
    return binary;
}

} // namespace lucid::parser