/**
 * @file ParseAttr.cpp
 * @brief The attribute parsers.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - parseAttributes      a juxtaposed sequence of `@name` / `@name(args)`
 *   - parseAttribute       one attribute (caller has consumed the `@`)
 *   - parseAttributeArg    one attribute argument
 *
 * ─── Design: attributes are juxtaposed ────────────────────────────────────
 * §9's grammar has no brackets and no commas between attributes:
 *
 *     @export @on(EventKind.KeyDown)
 *
 * is two attributes on one declaration. The parser reads them as a
 * sequence: each attribute starts with `@`; the sequence ends at the
 * first token that is not `@`.
 *
 * The old `@[...]` syntax is gone.
 *
 * ─── Design: attribute arguments are not general expressions ──────────────
 * An attribute argument is a literal, an identifier, or a dotted
 * identifier (§9's `attr_arg`). It is not an arbitrary expression: no
 * arithmetic, no calls, no array literals. The parser enforces this
 * directly — a token that cannot begin one of the permitted forms is
 * reported and skipped.
 *
 * The parser produces:
 *   - a literal argument    → LiteralExprAST
 *   - a bare identifier     → IdentifierExprAST
 *   - a dotted identifier   → a chain of FieldAccessExprAST
 *
 * For `EventKind.KeyDown`, the chain is:
 *   FieldAccessExprAST{ object = IdentifierExprAST("EventKind"),
 *                       fieldName = "KeyDown" }
 *
 * ─── Design: the parser does not validate attribute semantics ─────────────
 * The parser does not check that `@export` is a known attribute, that
 * `@on` takes a dotted identifier, or that `@deprecated` takes a string.
 * It produces the node and lets Sema validate. This keeps the parser from
 * having to know the attribute set, and keeps a typo in an attribute
 * name a name-resolution error rather than a parse error.
 */

#include "parser/Parser.hpp"
#include "parser/support/ErrorRecovery.hpp"

#include "core/Tokens.hpp"
#include "core/ast/ExprAST.hpp"

#include <vector>

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// parseAttributeArgIdent — a bare or dotted identifier
// =============================================================================

namespace {

/// @brief Parse an identifier or a dotted identifier as an attribute
///        argument.
///
/// The grammar's `attr_arg` allows `IDENTIFIER { '.' IDENTIFIER }`. A
/// single identifier produces an IdentifierExprAST; each `.` wraps the
/// current expression in a FieldAccessExprAST whose object is the
/// previous expression and whose fieldName is the identifier after the
/// dot.
///
/// @example
///   `EventKind`           → IdentifierExprAST("EventKind")
///   `EventKind.KeyDown`   → FieldAccessExprAST{
///                              object    = IdentifierExprAST("EventKind"),
///                              fieldName = "KeyDown"
///                          }
///   `a.b.c`               → FieldAccessExprAST{
///                              object    = FieldAccessExprAST{
///                                              object    = IdentifierExprAST("a"),
///                                              fieldName = "b"
///                                          },
///                              fieldName = "c"
///                          }
ExprAST* parseAttributeArgIdent(TokenStream& stream,
                                ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.check(TokenType::IDENTIFIER)) {
        // Caller checked; defensive only.
        return nullptr;
    }

    Token firstTok = stream.consume();
    ExprAST* expr = ctx.arena.make<IdentifierExprAST>(firstTok.value);
    expr->loc = loc;

    // Dotted chain.
    while (stream.check(TokenType::DOT)) {
        stream.consume();   // `.`

        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected an identifier after '.' in an "
                               "attribute argument, got '",
                               stream.peekValueView(ctx.pool), "'");
            expr->hasSyntaxError = true;
            break;
        }

        Token fieldTok = stream.consume();

        auto* access = ctx.arena.make<FieldAccessExprAST>(fieldTok.value);
        access->loc = expr->loc;
        access->object = expr;
        expr = access;
    }

    return expr;
}

} // namespace

// =============================================================================
// parseAttributes
// =============================================================================

ArenaSpan<AttributeAST*> parseAttributes(TokenStream& stream,
                                         ParserContext& ctx) {
    // If the current token is not `@`, the declaration has no
    // attributes. Return an empty span and consume nothing.
    if (!stream.check(TokenType::AT_SIGN)) {
        return ctx.arena.makeBuilder<AttributeAST*>().build();
    }

    std::vector<AttributeAST*> attrs;

    // Read a juxtaposed sequence of attributes. Each starts with `@`;
    // the sequence ends at the first token that is not `@`.
    while (!stream.isAtEnd() && stream.check(TokenType::AT_SIGN) &&
           ctx.canContinue()) {
        AttributeAST* attr = parseAttribute(stream, ctx);
        if (attr != nullptr) {
            attrs.push_back(attr);
        } else {
            // parseAttribute failed to produce even a marked node. It
            // reported a diagnostic. To avoid an infinite loop if it
            // did not advance, check the position.
            //
            // parseAttribute is expected to consume the `@` in every
            // path, so this branch is defensive.
            if (stream.check(TokenType::AT_SIGN)) {
                stream.consume();   // make progress
            }
        }
    }

    if (attrs.empty()) {
        return ctx.arena.makeBuilder<AttributeAST*>().build();
    }

    auto builder = ctx.arena.makeBuilder<AttributeAST*>(attrs.size());
    for (AttributeAST* a : attrs) builder.push_back(a);
    return builder.build();
}

// =============================================================================
// parseAttribute
// =============================================================================

AttributeAST* parseAttribute(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::AT_SIGN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '@' to start an attribute, got '",
                           stream.peekValueView(ctx.pool), "'");
        return nullptr;
    }

    // ─── Name ─────────────────────────────────────────────────────────────
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected an attribute name after '@', got '",
                           stream.peekValueView(ctx.pool), "'");

        // Partial-parse: produce an attribute with an empty name, marked.
        auto* placeholder = ctx.arena.make<AttributeAST>();
        placeholder->loc = loc;
        placeholder->name = InternedString{};   // invalid
        placeholder->hasSyntaxError = true;
        return placeholder;
    }

    Token nameTok = stream.consume();

    auto* attr = ctx.arena.make<AttributeAST>();
    attr->loc = loc;
    attr->name = nameTok.value;

    // ─── Optional argument list ───────────────────────────────────────────
    if (!stream.check(TokenType::LPAREN)) {
        return attr;   // no args: `@name`
    }

    stream.consume();   // `(`

    // Empty argument list: `@name()`.
    if (stream.match(TokenType::RPAREN)) {
        return attr;
    }

    std::vector<ExprAST*> args;

    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN) &&
           ctx.canContinue()) {
        ExprAST* arg = parseAttributeArg(stream, ctx);
        if (arg != nullptr) {
            args.push_back(arg);
        }

        if (stream.match(TokenType::COMMA)) {
            if (stream.check(TokenType::RPAREN)) {
                ctx.diag.errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in attribute arguments");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RPAREN)) break;

        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ')' in attribute arguments, got '",
                           stream.peekValueView(ctx.pool), "'");

        synchronizeUntil(stream, [](TokenType t) {
            return t == TokenType::COMMA || t == TokenType::RPAREN;
        });
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::RPAREN)) {
        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ')' to close the attribute arguments, "
                           "got '",
                           stream.peekValueView(ctx.pool), "'");
        attr->hasSyntaxError = true;
    }

    if (!args.empty()) {
        auto builder = ctx.arena.makeBuilder<ExprAST*>(args.size());
        for (ExprAST* a : args) builder.push_back(a);
        attr->args = builder.build();
    }

    return attr;
}

// =============================================================================
// parseAttributeArg
// =============================================================================

ExprAST* parseAttributeArg(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const TokenType current = stream.peekType();

    // ─── Literal ──────────────────────────────────────────────────────────
    //
    // The grammar's `attr_arg` lists `STRING_LIT | INT_LIT | FLOAT_LIT |
    // BOOL_LIT`. An `INT_LIT` can be decimal, hex, binary, or octal
    // (they are distinct token types). A `BOOL_LIT` is `true` or `false`.
    LiteralKind kind;
    switch (current) {
        case TokenType::STRING_LITERAL:     kind = LiteralKind::String; break;
        case TokenType::INT_LITERAL:        kind = LiteralKind::Int;    break;
        case TokenType::FLOAT_LITERAL:      kind = LiteralKind::Float;  break;
        case TokenType::HEX_LITERAL:        kind = LiteralKind::Hex;    break;
        case TokenType::BINARY_LITERAL:     kind = LiteralKind::Binary; break;
        case TokenType::OCTAL_LITERAL:      kind = LiteralKind::Octal;  break;
        case TokenType::KW_TRUE:            kind = LiteralKind::True;   break;
        case TokenType::KW_FALSE:           kind = LiteralKind::False;  break;

        case TokenType::IDENTIFIER:
            return parseAttributeArgIdent(stream, ctx);

        default:
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedLiteral, loc,
                               "expected a literal or identifier as an "
                               "attribute argument, got '",
                               stream.peekValueView(ctx.pool), "'");

            // Consume the offending token so the argument loop can make
            // progress, and return a marked placeholder.
            stream.consume();

            auto* unk = ctx.arena.make<UnknownExprAST>();
            unk->loc = loc;
            unk->hasSyntaxError = true;
            return unk;
    }

    Token litTok = stream.consume();
    auto* lit = ctx.arena.make<LiteralExprAST>(kind, litTok.value);
    lit->loc = loc;
    return lit;
}

} // namespace lucid::parser