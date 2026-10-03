/**
 * @file ParseExpr.cpp
 * @brief The expression parsers.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseExpr              entry point for the Pratt loop
 *   - parseRequiredExpr      parse or produce a marked UnknownExprAST
 *   - parsePrattExpr         the Pratt loop
 *   - parsePrefixExpr        unary operators and the primary dispatcher
 *   - parsePrimaryExpr       literals, identifiers, array literals,
 *                            parenthesized, lambdas, `start`
 *   - parsePostfixExpr       call, index, field access
 *   - parseLiteralExpr
 *   - parseIdentifierExpr
 *   - parseArrayLiteralExpr
 *   - parseParenExpr
 *   - parseLambdaExpr
 *   - parseStartExpr
 *   - parseCallExpr
 *   - parseIndexExpr
 *   - parseFieldAccessExpr
 *   - parseUnaryExpr
 *   - parseInfixBinary
 *   - looksLikeLambda
 *
 * ─── Design: the Pratt loop ───────────────────────────────────────────────
 * The Pratt loop is the standard top-down operator-precedence parser.
 * Each call parses a prefix form, then repeatedly:
 *
 *   - consumes a postfix operator (call, index, field access) if one is
 *     next, extending the left-hand side;
 *   - consumes an infix operator whose precedence is at least the loop's
 *     `minPrec`, parsing its right-hand side recursively.
 *
 * The "minimum precedence" is what makes precedence climbing work: each
 * recursive call sets a floor, and operators below the floor are left
 * for the enclosing call.
 *
 * ─── Design: assignment is not here ───────────────────────────────────────
 * §12: assignment is a statement, not an expression. The assignment
 * operators are not in the Pratt table; parseExpr stops cleanly before
 * them and the caller (parseAssignOrExprStmt in ParseStmt.cpp) handles
 * them. This is why this file has no parseInfixAssign.
 *
 * ─── Design: ranges are infix ─────────────────────────────────────────────
 * `..` and `..<` are infix operators. `parseInfixBinary` handles the whole
 * range construct: `lo..hi`, `lo..<hi`, `lo..hi..step`, and
 * `lo..<hi..step`. All four produce a single RangeExprAST whose `step`
 * field is null when no step was written.
 *
 * The grammar restricts where a range may appear — §6.12 says it is legal
 * only as a `for` iterable and as a `switch` case value. That is a Sema
 * check, not a parser check. The parser produces the shape; Sema enforces
 * the position. A range written in an illegal position (as a variable
 * initializer, say) is a range node that Sema rejects on type grounds.
 *
 * The optional step is written with a second range operator. A chained
 * range at general expression position (`0..10..2..3`) is parsed as
 * `((0..10..2)..3)` — a range whose lower bound is itself a range — and
 * rejected by Sema because a range is not a valid range bound.
 *
 * ─── Design: `and`/`or`/`not` are keywords ────────────────────────────────
 * They are keywords (§6.10), so the Pratt loop dispatches on their
 * token types like any other operator. No value-based dispatch is
 * needed.
 *
 * ─── Design: `**` is right-associative ────────────────────────────────────
 * `2 ** 3 ** 4` parses as `2 ** (3 ** 4)`. Every other binary operator is
 * left-associative. The infix handler for `**` recurses at the operator's
 * own precedence; every other operator recurses at precedence + 1.
 */

#include "parser/Parser.hpp"
#include "parser/support/ErrorRecovery.hpp"

#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <string>
#include <vector>

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// File-local operator tables
// =============================================================================
//
// These live here because the Pratt loop is their only consumer. They are
// not part of the parser's public interface and do not appear in
// Parser.hpp.

namespace {

/// @brief The binding power of prefix unary operators.
///
/// The grammar's §6.11 table puts unary operators at level 7. The operand
/// of a unary is parsed at this precedence, which lets a unary nest
/// (`- - x`) but stops it from consuming any infix operator (all of
/// which are below 7).
constexpr int kUnaryPrec = 7;

/// @brief The binding power of an infix operator, or -1 for a non-operator.
///
/// Higher numbers bind tighter. The values come directly from §6.11's
/// table. The two range operators (`..`, `..<`) are not in the table;
/// they get precedence 0, the same as `??`, because both are the loosest
/// operators and cannot meaningfully compose with each other.
int infixPrec(TokenType t) noexcept {
    switch (t) {
        case TokenType::POW:
            return 6;

        case TokenType::MUL:
        case TokenType::DIV:
        case TokenType::MOD:
            return 5;

        case TokenType::PLUS:
        case TokenType::MINUS:
            return 4;

        case TokenType::EQUAL_EQUAL:
        case TokenType::NOT_EQUAL:
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL:
            return 3;

        case TokenType::KW_AND:
            return 2;

        case TokenType::KW_OR:
            return 1;

        case TokenType::QUESTION_QUESTION:
        case TokenType::RANGE:
        case TokenType::RANGE_EXCLUSIVE:
            return 0;

        default:
            return -1;
    }
}

/// @brief Map an infix-operator token to its `BinaryOp`.
///
/// Precondition: the token is an infix operator other than the range
/// operators. The range operators are handled by `parseInfixBinary`
/// directly and never reach this function.
BinaryOp tokenToBinaryOp(TokenType t) noexcept {
    switch (t) {
        case TokenType::PLUS:              return BinaryOp::Add;
        case TokenType::MINUS:             return BinaryOp::Sub;
        case TokenType::MUL:               return BinaryOp::Mul;
        case TokenType::DIV:               return BinaryOp::Div;
        case TokenType::MOD:               return BinaryOp::Mod;
        case TokenType::POW:               return BinaryOp::Pow;
        case TokenType::EQUAL_EQUAL:       return BinaryOp::Eq;
        case TokenType::NOT_EQUAL:         return BinaryOp::Ne;
        case TokenType::LESS:              return BinaryOp::Lt;
        case TokenType::LESS_EQUAL:        return BinaryOp::Le;
        case TokenType::GREATER:           return BinaryOp::Gt;
        case TokenType::GREATER_EQUAL:     return BinaryOp::Ge;
        case TokenType::KW_AND:            return BinaryOp::And;
        case TokenType::KW_OR:             return BinaryOp::Or;
        case TokenType::BIT_AND:           return BinaryOp::BitAnd;
        case TokenType::BIT_OR:            return BinaryOp::BitOr;
        case TokenType::BIT_XOR:           return BinaryOp::BitXor;
        case TokenType::SHL:               return BinaryOp::Shl;
        case TokenType::SHR:               return BinaryOp::Shr;
        case TokenType::QUESTION_QUESTION: return BinaryOp::NullCoalesce;

        default:
            // Unreachable: the caller dispatches range operators first,
            // and `infixPrec` returns -1 for everything else.
            return BinaryOp::Add;
    }
}

} // namespace

// =============================================================================
// parseExpr — the entry point
// =============================================================================

ExprAST* parseExpr(TokenStream& stream, ParserContext& ctx) {
    if (stream.isAtEnd()) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected an expression, but the input ended");
        return nullptr;
    }

    return parsePrattExpr(stream, ctx, /*minPrec=*/-1);
}

// =============================================================================
// parseRequiredExpr — parse or produce a marked placeholder
// =============================================================================

ExprAST* parseRequiredExpr(TokenStream& stream,
                           ParserContext& ctx,
                           const char* expectedWhat) {
    ExprAST* expr = parseExpr(stream, ctx);
    if (expr != nullptr) return expr;

    // parseExpr already reported a diagnostic. Produce a marked
    // UnknownExprAST so the caller can continue without a null check at
    // every use site.
    (void)expectedWhat;

    auto* placeholder = ctx.arena.make<UnknownExprAST>();
    placeholder->loc = stream.currentLoc();
    placeholder->hasSyntaxError = true;
    return placeholder;
}

// =============================================================================
// parsePrattExpr — the loop
// =============================================================================

ExprAST* parsePrattExpr(TokenStream& stream,
                        ParserContext& ctx,
                        int minPrec) {
    // ─── Prefix ───────────────────────────────────────────────────────────
    ExprAST* lhs = parsePrefixExpr(stream, ctx);
    if (lhs == nullptr) return nullptr;

    // ─── Loop over postfix and infix operators ────────────────────────────
    while (!stream.isAtEnd()) {
        const TokenType current = stream.peekType();

        // ─── Postfix ──────────────────────────────────────────────────────
        //
        // Postfix operators bind tighter than any infix. They extend the
        // current left-hand side: a call, an index, or a field access.
        if (current == TokenType::LPAREN ||
            current == TokenType::LBRACKET ||
            current == TokenType::DOT) {
            lhs = parsePostfixExpr(stream, ctx, lhs);
            if (lhs == nullptr) return nullptr;
            continue;
        }

        // ─── Infix operators ──────────────────────────────────────────────
        const int prec = infixPrec(current);
        if (prec < 0 || prec < minPrec) break;

        const TokenType opTok = current;
        stream.consume();

        lhs = parseInfixBinary(stream, ctx, lhs, opTok, prec);
        if (lhs == nullptr) return nullptr;
    }

    return lhs;
}

// =============================================================================
// parsePrefixExpr — unary operators and the primary dispatcher
// =============================================================================

ExprAST* parsePrefixExpr(TokenStream& stream, ParserContext& ctx) {
    const TokenType current = stream.peekType();

    switch (current) {
        case TokenType::MINUS:
            stream.consume();
            return parseUnaryExpr(stream, ctx, UnaryOp::Neg);

        case TokenType::BIT_NOT:
            stream.consume();
            return parseUnaryExpr(stream, ctx, UnaryOp::BitNot);

        case TokenType::KW_NOT:
            stream.consume();
            return parseUnaryExpr(stream, ctx, UnaryOp::Not);

        default:
            return parsePrimaryExpr(stream, ctx);
    }
}

// =============================================================================
// parsePrimaryExpr — the primary forms
// =============================================================================

ExprAST* parsePrimaryExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // ─── Literals ─────────────────────────────────────────────────────────
    if (isLiteral(current)) {
        return parseLiteralExpr(stream, ctx);
    }

    // ─── Array literal ────────────────────────────────────────────────────
    if (current == TokenType::LBRACKET) {
        return parseArrayLiteralExpr(stream, ctx);
    }

    // ─── Identifier ───────────────────────────────────────────────────────
    if (current == TokenType::IDENTIFIER) {
        return parseIdentifierExpr(stream, ctx);
    }

    // ─── `(` — parenthesized expression or lambda ─────────────────────────
    if (current == TokenType::LPAREN) {
        if (looksLikeLambda(stream, ctx)) {
            return parseLambdaExpr(stream, ctx);
        }
        return parseParenExpr(stream, ctx);
    }

    // ─── `start` expression ───────────────────────────────────────────────
    if (current == TokenType::KW_START) {
        return parseStartExpr(stream, ctx);
    }

    // ─── Not a primary ────────────────────────────────────────────────────
    ctx.diag.errorAt(DiagCode::Syntax_ExpectedExpression, loc,
                       "expected an expression, got '",
                       stream.peekValueView(ctx.pool), "'");
    return nullptr;
}

// =============================================================================
// parsePostfixExpr — call, index, field access
// =============================================================================

ExprAST* parsePostfixExpr(TokenStream& stream, ParserContext& ctx,
                          ExprAST* lhs) {
    if (lhs == nullptr) return nullptr;

    const TokenType current = stream.peekType();

    if (current == TokenType::LPAREN) {
        return parseCallExpr(stream, ctx, lhs);
    }
    if (current == TokenType::LBRACKET) {
        return parseIndexExpr(stream, ctx, lhs);
    }
    if (current == TokenType::DOT) {
        return parseFieldAccessExpr(stream, ctx, lhs);
    }

    // The Pratt loop checks the postfix-start set before calling, so this
    // branch is unreachable. Return lhs unchanged as a defensive fallback.
    return lhs;
}

// =============================================================================
// parseLiteralExpr
// =============================================================================

LiteralExprAST* parseLiteralExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    LiteralKind kind;
    switch (current) {
        case TokenType::INT_LITERAL:        kind = LiteralKind::Int;       break;
        case TokenType::FLOAT_LITERAL:      kind = LiteralKind::Float;     break;
        case TokenType::HEX_LITERAL:        kind = LiteralKind::Hex;       break;
        case TokenType::BINARY_LITERAL:     kind = LiteralKind::Binary;    break;
        case TokenType::OCTAL_LITERAL:      kind = LiteralKind::Octal;     break;
        case TokenType::CHAR_LITERAL:       kind = LiteralKind::Char;      break;
        case TokenType::STRING_LITERAL:     kind = LiteralKind::String;    break;
        case TokenType::RAW_STRING_LITERAL: kind = LiteralKind::RawString; break;
        case TokenType::KW_TRUE:            kind = LiteralKind::True;      break;
        case TokenType::KW_FALSE:           kind = LiteralKind::False;     break;
        case TokenType::KW_NIL:             kind = LiteralKind::Nil;       break;
        default:
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedLiteral, loc,
                               "expected a literal, got '",
                               stream.peekValueView(ctx.pool), "'");
            return nullptr;
    }

    Token litTok = stream.consume();
    auto* lit = ctx.arena.make<LiteralExprAST>(kind, litTok.value);
    lit->loc = loc;
    return lit;
}

// =============================================================================
// parseIdentifierExpr
// =============================================================================

IdentifierExprAST* parseIdentifierExpr(TokenStream& stream,
                                       ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier, loc,
                           "expected an identifier, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    Token nameTok = stream.consume();
    auto* id = ctx.arena.make<IdentifierExprAST>(nameTok.value);
    id->loc = loc;
    return id;
}

// =============================================================================
// parseArrayLiteralExpr
// =============================================================================

ArrayLiteralExprAST* parseArrayLiteralExpr(TokenStream& stream,
                                           ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '[' for an array literal, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // Empty array: `[]`.
    if (stream.match(TokenType::RBRACKET)) {
        auto* lit = ctx.arena.make<ArrayLiteralExprAST>(
            ctx.arena.makeBuilder<ExprAST*>().build());
        lit->loc = loc;
        return lit;
    }

    std::vector<ExprAST*> elements;

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET) &&
           ctx.canContinue()) {
        ExprAST* e = parseRequiredExpr(stream, ctx, "array element");
        elements.push_back(e);

        if (stream.match(TokenType::COMMA)) {
            if (stream.check(TokenType::RBRACKET)) {
                ctx.diag.errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in array literal");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RBRACKET)) break;

        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ']' in array literal, got '",
                           stream.peekValueView(ctx.pool), "'");

        synchronizeUntil(stream, [](TokenType t) {
            return t == TokenType::COMMA || t == TokenType::RBRACKET;
        });
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close the array literal, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    auto b = ctx.arena.makeBuilder<ExprAST*>(elements.size());
    for (ExprAST* e : elements) b.push_back(e);

    auto* lit = ctx.arena.make<ArrayLiteralExprAST>(b.build());
    lit->loc = loc;
    return lit;
}

// =============================================================================
// parseParenExpr
// =============================================================================

ParenExprAST* parseParenExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '(', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    ExprAST* inner = parseRequiredExpr(stream, ctx, "parenthesized expression");

    if (!stream.match(TokenType::RPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ')' to close the parenthesized "
                           "expression, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    auto* paren = ctx.arena.make<ParenExprAST>(inner);
    paren->loc = loc;
    if (inner != nullptr && inner->hasSyntaxError) {
        paren->hasSyntaxError = true;
    }
    return paren;
}

// =============================================================================
// looksLikeLambda
// =============================================================================

bool looksLikeLambda(TokenStream& stream, ParserContext& ctx) {
    (void)ctx;
    const size_t savedPos = stream.getPos();

    if (!stream.check(TokenType::LPAREN)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume();   // `(`

    // Skip to the matching `)`. Track depth so nested parentheses (in
    // parameter types, like a function-typed parameter) do not confuse
    // the scan.
    int depth = 1;
    while (!stream.isAtEnd()) {
        const TokenType t = stream.peekType();
        if (t == TokenType::LPAREN) {
            depth++;
            stream.consume();
            continue;
        }
        if (t == TokenType::RPAREN) {
            depth--;
            stream.consume();
            if (depth == 0) break;
            continue;
        }
        stream.consume();
    }

    const bool result = !stream.isAtEnd() &&
                        stream.check(TokenType::ARROW);

    stream.setPos(savedPos);
    return result;
}

// =============================================================================
// parseLambdaExpr
// =============================================================================

LambdaExprAST* parseLambdaExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '(' to open the lambda parameters, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // parseParamList consumes the `( params )` group itself.
    std::vector<ParamAST*> params = parseParamList(stream, ctx);

    auto* lambda = ctx.arena.make<LambdaExprAST>();
    lambda->loc = loc;

    if (!params.empty()) {
        auto b = ctx.arena.makeBuilder<ParamAST*>(params.size());
        for (ParamAST* p : params) b.push_back(p);
        lambda->params = b.build();
    }

    if (!stream.match(TokenType::ARROW)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '->' after the lambda parameters, got '",
                           stream.peekValueView(ctx.pool), "'");
        lambda->hasSyntaxError = true;
        return lambda;
    }

    ExprAST* body = parseRequiredExpr(stream, ctx, "lambda body");
    lambda->body = body;
    if (body != nullptr && body->hasSyntaxError) {
        lambda->hasSyntaxError = true;
    }
    return lambda;
}

// =============================================================================
// parseStartExpr
// =============================================================================

StartExprAST* parseStartExpr(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::KW_START)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected 'start', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    ExprAST* operand = parseRequiredExpr(stream, ctx, "call expression");

    if (operand == nullptr || !operand->isa<CallExprAST>()) {
        if (operand != nullptr && !operand->hasSyntaxError) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, operand->loc,
                               "'start' must be followed by a call "
                               "expression");
        }
        auto* start = ctx.arena.make<StartExprAST>(nullptr);
        start->loc = loc;
        start->hasSyntaxError = true;
        return start;
    }

    auto* start = ctx.arena.make<StartExprAST>(operand->as<CallExprAST>());
    start->loc = loc;
    return start;
}

// =============================================================================
// parseCallExpr
// =============================================================================

CallExprAST* parseCallExpr(TokenStream& stream, ParserContext& ctx,
                           ExprAST* callee) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '(' to open the argument list, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    ArenaSpan<ExprAST*> args = parseArgList(stream, ctx);

    auto* call = ctx.arena.make<CallExprAST>();
    call->loc = loc;
    call->callee = callee;
    call->args = args;
    return call;
}

// =============================================================================
// parseIndexExpr
// =============================================================================

IndexExprAST* parseIndexExpr(TokenStream& stream, ParserContext& ctx,
                             ExprAST* target) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '[' to open the index, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    ExprAST* index = parseRequiredExpr(stream, ctx, "index expression");

    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close the index, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    auto* idx = ctx.arena.make<IndexExprAST>(target, index);
    idx->loc = loc;
    return idx;
}

// =============================================================================
// parseFieldAccessExpr
// =============================================================================

FieldAccessExprAST* parseFieldAccessExpr(TokenStream& stream,
                                         ParserContext& ctx,
                                         ExprAST* object) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::DOT)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '.', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a field name after '.', got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    Token fieldTok = stream.consume();

    auto* access = ctx.arena.make<FieldAccessExprAST>(fieldTok.value);
    access->loc = loc;
    access->object = object;
    return access;
}

// =============================================================================
// parseUnaryExpr
// =============================================================================

UnaryExprAST* parseUnaryExpr(TokenStream& stream, ParserContext& ctx,
                             UnaryOp op) {
    // The operator itself has already been consumed by parsePrefixExpr;
    // previousLoc() is its location.
    const SourceLocation loc = stream.previousLoc();

    // The operand is parsed at unary precedence. It can be another unary
    // (`- - x`), a primary, or a postfix chain. It cannot consume any
    // infix operator, because every infix operator's precedence is below
    // unary precedence.
    ExprAST* operand = parsePrattExpr(stream, ctx, kUnaryPrec);

    if (operand == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected an operand for the unary operator");

        auto* unk = ctx.arena.make<UnknownExprAST>();
        unk->loc = stream.currentLoc();
        unk->hasSyntaxError = true;

        auto* unary = ctx.arena.make<UnaryExprAST>(op);
        unary->loc = loc;
        unary->operand = unk;
        unary->hasSyntaxError = true;
        return unary;
    }

    auto* unary = ctx.arena.make<UnaryExprAST>(op);
    unary->loc = loc;
    unary->operand = operand;
    if (operand->hasSyntaxError) unary->hasSyntaxError = true;
    return unary;
}

// =============================================================================
// parseInfixBinary
// =============================================================================

ExprAST* parseInfixBinary(TokenStream& stream, ParserContext& ctx,
                          ExprAST* lhs, TokenType opTok, int prec) {
    // ─── Range operators ──────────────────────────────────────────────────
    //
    // A range is `lo..hi`, `lo..<hi`, `lo..hi..step`, or
    // `lo..<hi..step`. The step, if written, uses a second range operator.
    // All four forms produce a single RangeExprAST with `step` null when
    // no step was written.
    //
    // The bounds (and the step) are parsed at `prec + 1`, so they do not
    // themselves consume the next range operator. That is what lets
    // `parseInfixBinary` see the second range operator and attach it as
    // the step.
    if (opTok == TokenType::RANGE || opTok == TokenType::RANGE_EXCLUSIVE) {
        const bool isExclusive = (opTok == TokenType::RANGE_EXCLUSIVE);

        // Upper bound.
        ExprAST* hi = parsePrattExpr(stream, ctx, prec + 1);
        if (hi == nullptr) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedRangeBound,
                               stream.currentLoc(),
                               "expected an upper bound for the range");
            hi = ctx.arena.make<UnknownExprAST>();
            hi->loc = stream.currentLoc();
            hi->hasSyntaxError = true;
        }

        auto* range = ctx.arena.make<RangeExprAST>(isExclusive);
        range->loc = lhs->loc;
        range->lo = lhs;
        range->hi = hi;

        // Optional step: a second range operator.
        if (stream.check(TokenType::RANGE) ||
            stream.check(TokenType::RANGE_EXCLUSIVE)) {
            stream.consume();

            ExprAST* step = parsePrattExpr(stream, ctx, prec + 1);
            if (step == nullptr) {
                ctx.diag.errorAt(
                    DiagCode::Syntax_ExpectedRangeBound,
                    stream.currentLoc(),
                    "expected a step expression after the range");
                step = ctx.arena.make<UnknownExprAST>();
                step->loc = stream.currentLoc();
                step->hasSyntaxError = true;
            }
            range->step = step;
        }

        return range;
    }

    // ─── Every other infix operator ───────────────────────────────────────
    const BinaryOp op = tokenToBinaryOp(opTok);
    const bool rightAssoc = (op == BinaryOp::Pow);

    ExprAST* rhs = parsePrattExpr(stream, ctx,
                                  rightAssoc ? prec : prec + 1);
    if (rhs == nullptr) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedExpression,
                           stream.currentLoc(),
                           "expected the right-hand side of '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    auto* binary = ctx.arena.make<BinaryExprAST>(op);
    binary->loc = lhs->loc;
    binary->left = lhs;
    binary->right = rhs;
    return binary;
}

} // namespace lucid::parser