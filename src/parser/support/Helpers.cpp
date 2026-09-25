/**
 * @file Helpers.cpp
 * @brief Shared utility parsers used across the parser.
 *
 * ─── What this file implements ────────────────────────────────────────────
 * Every helper declared in Parser.hpp's section 9 (Helpers). Grouped by
 * role:
 *
 *   Doc comments and attributes:
 *     - harvestDocComment           scan backward for a doc comment
 *     - parseAttributes             `@[a, b, c]`
 *     - parseAttribute              one `@[a]` item
 *     - parseAttributeArgLiteral    one argument inside an attribute
 *
 *   Generic parameters and arguments:
 *     - parseGenericParamDecl       one `<T : Trait>` entry
 *     - parseGenericParamDecls      the whole `<...>` list
 *     - parseGenericArgs            the `<T, U>` list at a use site
 *
 *   Argument and parameter lists:
 *     - parseArgList                `(a, b, c)`
 *     - parseParamList              `(a T, b U)`
 *     - parseSingleParameter        one parameter
 *
 *   Import paths:
 *     - parseImportPath             `a.b.c`
 *
 *   Trait references:
 *     - parseTraitRefList           `A, B, C`
 *
 *   Host target sigils:
 *     - parseHostTarget             `#host(name)` / `#native` / `#builtin`
 *
 *   Small shared utilities:
 *     - consumeDeclarationSemicolon
 *     - consumeSubDeclSemicolon
 *     - makeFuncType
 *     - startsStructFieldItem
 *     - startsEnumVariantItem
 *
 * ─── Design: list parsers, item parsers, and separators ───────────────────
 * Several helpers come in pairs: a list parser that consumes the whole
 * `( ... )` or `< ... >` sequence, and an item parser that parses one
 * element inside the list. The list parser owns the separators (commas);
 * the item parser does not consume them. This is a clean separation and
 * every list in this file follows it.
 *
 * ─── Design: comma handling ───────────────────────────────────────────────
 * The list parsers tolerate trailing commas in some contexts and reject
 * them in others, according to the grammar. Where the grammar permits a
 * trailing comma (struct field lists, enum variant lists, argument lists
 * in some positions), the parser consumes it silently. Where it does not
 * (function parameter lists, generic parameter lists), the parser reports
 * a diagnostic.
 *
 * The rule for a specific list is stated in the function's own comment.
 *
 * ─── Design: the doc-comment harvester scans backward ─────────────────────
 * `harvestDocComment` scans *backward* from the current stream position,
 * through the raw token vector, to find the comment that precedes the
 * declaration the parser is about to read. It cannot use the forward
 * stream because the parser has already consumed past the comments by
 * the time the harvester runs; the tokens are visible only through the
 * stream's `getTokens()` / `getPos()` accessors.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <vector>

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// 9.1 Doc comments and attributes
// =============================================================================

// ─── harvestDocComment ──────────────────────────────────────────────────

/// @brief Recover the doc comment attached to the declaration at the
///        current stream position.
///
/// A doc comment is either:
///
///   - a `/-- ... --/` block comment, emitted by the lexer as a
///     DOC_COMMENT token, or
///   - a run of `--` line comments above the declaration.
///
/// Both forms appear in the token vector; the lexer drops `--` line
/// comments but keeps `/-- ... --/` as DOC_COMMENT. Wait — under the
/// current grammar, `--` is a line comment that the lexer *does* keep
/// (as a LINE_COMMENT token) or drops (depending on the lexer's
/// contract). See the note in the function.
///
/// The harvester scans backward from the current stream position. It
/// stops at the first non-comment token, or when the distance between
/// the comment and the declaration exceeds the "attached" threshold
/// (consecutive lines).
///
/// The returned DocComment carries the comment text (with the comment
/// markers stripped) and the form the comment was written in. A
/// declaration that has no attached comment returns std::nullopt.
std::optional<DocComment> harvestDocComment(TokenStream& stream,
                                            ParserContext& ctx) {
    const auto& tokens = stream.getTokens();
    const size_t pos = stream.getPos();
    if (pos == 0) return std::nullopt;

    // The declaration's line. Used to detect "comment on the same line"
    // (trailing form) and "comment on the previous line" (stacked form).
    const uint32_t declLine = stream.peek().location.line();

    std::optional<std::string> trailingText;
    std::vector<std::string>   stackedLines;
    uint32_t                   stackedTopLine = 0;
    bool                       hasStackedTopLine = false;
    std::optional<std::string> blockText;

    // Scan backward through the raw token vector. The vector includes
    // the comment tokens the forward stream skips, so this is where
    // they are reachable.
    for (size_t i = pos; i > 0; ) {
        --i;
        const Token& t = tokens[i];

        if (t.type == TokenType::LINE_COMMENT) {
            if (t.location.line() == 0) continue;   // malformed; skip

            if (t.location.line() == declLine) {
                // A comment on the same line as the declaration.
                // This is the *trailing* form. Only the first such
                // comment counts; subsequent same-line comments are
                // ignored (they cannot be on the same line and
                // precede the declaration in any meaningful order).
                if (!trailingText.has_value()) {
                    trailingText = t.value;
                }
                continue;
            }

            if (stackedLines.empty()) {
                // First comment above the declaration. It must be on
                // the immediately preceding line, or it is not
                // attached.
                if (declLine - t.location.line() == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.location.line();
                    hasStackedTopLine = true;
                    continue;
                } else {
                    break;
                }
            } else {
                // Another stacked comment. It must be on the line
                // immediately above the previous one.
                if (hasStackedTopLine &&
                    stackedTopLine - t.location.line() == 1) {
                    stackedLines.push_back(t.value);
                    stackedTopLine = t.location.line();
                    continue;
                } else {
                    break;
                }
            }
        }

        if (t.type == TokenType::DOC_COMMENT) {
            if (t.location.line() == 0) continue;

            // The block form. It must be on the declaration's line or
            // the line immediately above it. (A block comment spanning
            // multiple lines is one token; its location is its start.)
            if (declLine - t.location.line() <= 1) {
                blockText = t.value;
            }
            break;
        }

        // Anything else terminates the scan. The declaration is the
        // first non-comment token, so anything else is a preceding
        // declaration or an unrelated token.
        break;
    }

    // Priority: block > stacked > trailing.
    //
    // A block comment and a stacked-line run cannot both be attached
    // in practice; the scan stops at the first non-comment token, so
    // only one form reaches this point. The order is defensive.
    if (blockText.has_value()) {
        return DocComment{ctx.pool().intern(*blockText), DocCommentForm::Block};
    }

    if (!stackedLines.empty()) {
        // The stacked lines were collected bottom-to-top. Reverse them
        // so the joined text reads top-to-bottom, matching the source.
        std::string combined;
        for (auto it = stackedLines.rbegin(); it != stackedLines.rend(); ++it) {
            if (!combined.empty()) combined += '\n';
            combined += *it;
        }
        return DocComment{ctx.pool().intern(combined), DocCommentForm::Stacked};
    }

    if (trailingText.has_value()) {
        return DocComment{ctx.pool().intern(*trailingText),
                          DocCommentForm::Trailing};
    }

    return std::nullopt;
}

// ─── parseAttributes ────────────────────────────────────────────────────

/// @brief Parse an optional `@[attr, attr, ...]` list.
///
/// If the current token is not `@`, returns an empty span and consumes
/// nothing. Otherwise consumes the `@`, the `[`, the comma-separated
/// attributes, and the `]`.
///
/// The list may be empty: `@[]` is accepted (it parses as a list of
/// zero attributes). In practice Sema will reject an empty list as
/// having no effect, but the parser does not enforce that.
ArenaSpan<AttributeAST*> parseAttributes(TokenStream& stream,
                                         ParserContext& ctx) {
    // No `@`: no attribute list. Return an empty span.
    if (!stream.check(TokenType::AT_SIGN)) {
        return ctx.arena().makeBuilder<AttributeAST*>().build();
    }
    stream.consume();   // `@`

    if (!stream.match(TokenType::LBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '[' after '@', got '",
                           stream.peekValue(), "'");
        // Recover: no attributes.
        return ctx.arena().makeBuilder<AttributeAST*>().build();
    }

    std::vector<AttributeAST*> attrs;

    // Empty list: `@[]`.
    if (stream.match(TokenType::RBRACKET)) {
        return ctx.arena().makeBuilder<AttributeAST*>().build();
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RBRACKET) &&
           ctx.canContinue()) {
        // Parse one attribute. parseAttribute does not consume the
        // comma after it; the list loop handles the comma.
        AttributeAST* attr = parseAttribute(stream, ctx);
        if (attr) attrs.push_back(attr);

        // Comma or closing bracket.
        if (stream.match(TokenType::COMMA)) {
            continue;
        }
        if (stream.check(TokenType::RBRACKET)) {
            break;
        }

        // Neither: error and synchronize to the next attribute or the
        // closing bracket.
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ']' in attribute list, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::COMMA,
                      TokenType::RBRACKET);
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    // Closing `]`.
    if (!stream.match(TokenType::RBRACKET)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ']' to close attribute list, got '",
                           stream.peekValue(), "'");
    }

    auto builder = ctx.arena().makeBuilder<AttributeAST*>(attrs.size());
    for (AttributeAST* a : attrs) builder.push_back(a);
    return builder.build();
}

// ─── parseAttribute ─────────────────────────────────────────────────────

/// @brief Parse one attribute: `name` or `name(arg, arg, ...)`.
///
/// The caller has consumed the `@[` (or the preceding comma). This
/// function consumes the attribute's name, its optional argument list,
/// and nothing else. It does not consume the comma that separates it
/// from the next attribute; the list parser handles that.
AttributeAST* parseAttribute(TokenStream& stream, ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           loc,
                           "expected attribute name, got '",
                           stream.peekValue(), "'");
        // Recover: produce an empty attribute with an error flag.
        auto* placeholder = ctx.arena().make<AttributeAST>();
        placeholder->loc = loc;
        placeholder->name = ctx.pool().intern("");
        placeholder->hasSyntaxError = true;
        return placeholder;
    }

    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    auto* attr = ctx.arena().make<AttributeAST>();
    attr->loc = loc;
    attr->name = name;

    // Optional argument list.
    if (stream.match(TokenType::LPAREN)) {
        std::vector<LiteralExprAST*> args;

        // Empty argument list: `@[name()]`.
        if (stream.match(TokenType::RPAREN)) {
            attr->args = ctx.arena().makeBuilder<LiteralExprAST*>().build();
            return attr;
        }

        while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN) &&
               ctx.canContinue()) {
            LiteralExprAST* arg = parseAttributeArgLiteral(stream, ctx);
            if (arg) args.push_back(arg);

            if (stream.match(TokenType::COMMA)) {
                continue;
            }
            if (stream.check(TokenType::RPAREN)) {
                break;
            }

            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ',' or ')' in attribute arguments, "
                               "got '", stream.peekValue(), "'");
            synchronizeTo(stream, ctx,
                          TokenType::COMMA,
                          TokenType::RPAREN);
            if (stream.match(TokenType::COMMA)) continue;
            break;
        }

        if (!stream.match(TokenType::RPAREN)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                               stream.currentLoc(),
                               "expected ')' to close attribute arguments, "
                               "got '", stream.peekValue(), "'");
        }

        auto builder = ctx.arena().makeBuilder<LiteralExprAST*>(args.size());
        for (LiteralExprAST* a : args) builder.push_back(a);
        attr->args = builder.build();
    }

    return attr;
}

// ─── parseAttributeArgLiteral ───────────────────────────────────────────

/// @brief Parse one argument inside an attribute's parentheses.
///
/// Attribute arguments are restricted to literals: strings, integers,
/// floats, chars, booleans, and bare identifiers. The grammar's
/// `attr_arg` production lists these forms; a full expression is not
/// allowed.
///
/// The literal is produced as a `LiteralExprAST` with the raw lexeme as
/// its value. Sema interprets the lexeme for the specific attribute
/// that requires it.
LiteralExprAST* parseAttributeArgLiteral(TokenStream& stream,
                                         ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();
    const Token tok = stream.peek();

    LiteralKind kind;
    switch (tok.type) {
        case TokenType::STRING_HEAD: {
            // A string literal in the token stream is a sequence of
            // STRING_HEAD / STRING_MIDDLE / STRING_END. An attribute
            // argument uses the simple form (one HEAD, one END, no
            // interpolation). If an interpolation appears, the parser
            // reports an error and skips to the end of the string.
            stream.consume();   // STRING_HEAD
            if (stream.check(TokenType::STRING_END)) {
                stream.consume();   // STRING_END
                auto* lit = ctx.arena().make<LiteralExprAST>(
                    LiteralKind::String, ctx.pool().intern(tok.value));
                lit->loc = loc;
                return lit;
            }
            // Interpolation inside an attribute string is not allowed.
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedLiteral, loc,
                               "an attribute argument string cannot contain "
                               "an interpolation");
            while (!stream.isAtEnd() &&
                   !stream.check(TokenType::STRING_END)) {
                stream.consume();
            }
            if (stream.check(TokenType::STRING_END)) stream.consume();
            auto* lit = ctx.arena().make<LiteralExprAST>(
                LiteralKind::String, ctx.pool().intern(tok.value));
            lit->loc = loc;
            lit->hasSyntaxError = true;
            return lit;
        }

        case TokenType::RAW_STRING_LITERAL:
            kind = LiteralKind::RawString;
            break;

        case TokenType::INT_LITERAL:
        case TokenType::HEX_LITERAL:
        case TokenType::BINARY_LITERAL:
            kind = (tok.type == TokenType::INT_LITERAL) ? LiteralKind::Int
                 : (tok.type == TokenType::HEX_LITERAL) ? LiteralKind::Hex
                                                        : LiteralKind::Binary;
            break;

        case TokenType::FLOAT_LITERAL:
            kind = LiteralKind::Float;
            break;

        case TokenType::CHAR_LITERAL:
            kind = LiteralKind::Char;
            break;

        case TokenType::KW_TRUE:
            kind = LiteralKind::True;
            break;

        case TokenType::KW_FALSE:
            kind = LiteralKind::False;
            break;

        case TokenType::IDENTIFIER:
            // A bare identifier as an attribute argument. Used for
            // arguments that name a mode or a target. Stored as a
            // String literal with the identifier's text as value.
            kind = LiteralKind::String;
            break;

        default:
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedLiteral,
                               loc,
                               "expected a literal attribute argument, got '",
                               stream.peekValue(), "'");
            // Recover: produce an empty literal with an error flag.
            stream.consume();   // consume the offending token
            auto* placeholder = ctx.arena().make<LiteralExprAST>(
                LiteralKind::Unknown, ctx.pool().intern(""));
            placeholder->loc = loc;
            placeholder->hasSyntaxError = true;
            return placeholder;
    }

    // Simple literal: consume the token and produce the node.
    Token valueTok = stream.consume();
    auto* lit = ctx.arena().make<LiteralExprAST>(
        kind, ctx.pool().intern(valueTok.value));
    lit->loc = loc;
    return lit;
}

// =============================================================================
// 9.2 Generic parameters and arguments
// =============================================================================

// ─── parseGenericParamDecl ──────────────────────────────────────────────

/// @brief Parse one `<T>` or `<T : Trait1 + Trait2>` entry.
///
/// The caller (the list parser) has already consumed the `<` or a comma.
/// This function consumes the parameter's name, its optional
/// constraints, and stops at the comma or `>` that follows. The list
/// parser handles the separator.
GenericParamDeclAST* parseGenericParamDecl(TokenStream& stream,
                                           ParserContext& ctx) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           loc,
                           "expected a generic parameter name, got '",
                           stream.peekValue(), "'");
        // Recover: empty parameter.
        auto* placeholder = ctx.arena().make<GenericParamDeclAST>(
            ctx.pool().intern(""));
        placeholder->loc = loc;
        placeholder->hasSyntaxError = true;
        return placeholder;
    }

    Token nameTok = stream.consume();
    InternedString name = ctx.pool().intern(nameTok.value);

    auto* param = ctx.arena().make<GenericParamDeclAST>(name);
    param->loc = loc;

    // Optional constraints: `: Trait1 + Trait2`.
    if (stream.match(TokenType::COLON)) {
        std::vector<NamedTypeAST*> constraints;

        while (!stream.isAtEnd()) {
            // A constraint is a named type, possibly with generic
            // arguments: `Container<int>`, `Eq`, `Ord`.
            TypeAST* parsed = parseNamedType(stream, ctx);
            if (!parsed || !parsed->isa<NamedTypeAST>()) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                                   stream.currentLoc(),
                                   "expected a trait constraint, got '",
                                   stream.peekValue(), "'");
                // If a comma or `>` follows, stop the constraint list.
                // Otherwise skip a token to make progress.
                if (stream.check(TokenType::COMMA) ||
                    stream.check(TokenType::GREATER)) {
                    break;
                }
                synchronizeTo(stream, ctx,
                              TokenType::PLUS,
                              TokenType::COMMA,
                              TokenType::GREATER);
                if (stream.match(TokenType::PLUS)) continue;
                break;
            }
            constraints.push_back(parsed->as<NamedTypeAST>());

            // `+` continues the constraint list; anything else ends it.
            if (!stream.match(TokenType::PLUS)) {
                break;
            }
        }

        auto builder = ctx.arena().makeBuilder<NamedTypeAST*>(constraints.size());
        for (NamedTypeAST* c : constraints) builder.push_back(c);
        param->constraints = builder.build();

        if (param->constraints.empty()) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected a trait constraint after ':'");
            param->hasSyntaxError = true;
        }
    }

    return param;
}

// ─── parseGenericParamDecls ─────────────────────────────────────────────

/// @brief Parse the full `<T, U, V>` list at a declaration site.
///
/// If the current token is not `<`, returns an empty span and consumes
/// nothing. Otherwise consumes the `<`, the parameters (separated by
/// commas), and the `>`.
///
/// Empty `<>` is a syntax error: a generic parameter list with no
/// parameters has no meaning, and the grammar requires at least one.
ArenaSpan<GenericParamDeclAST*> parseGenericParamDecls(TokenStream& stream,
                                                       ParserContext& ctx) {
    // No `<`: no generic parameters. Return an empty span.
    if (!stream.check(TokenType::LESS)) {
        return ctx.arena().makeBuilder<GenericParamDeclAST*>().build();
    }
    stream.consume();   // `<`

    // Empty `<>`.
    if (stream.check(TokenType::GREATER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "a generic parameter list must not be empty; "
                           "remove the '<>' or add a parameter");
        stream.consume();   // `>`
        return ctx.arena().makeBuilder<GenericParamDeclAST*>().build();
    }

    std::vector<GenericParamDeclAST*> params;

    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER) &&
           ctx.canContinue()) {
        GenericParamDeclAST* param = parseGenericParamDecl(stream, ctx);
        if (param) params.push_back(param);

        if (stream.match(TokenType::COMMA)) {
            continue;
        }
        if (stream.check(TokenType::GREATER)) {
            break;
        }

        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or '>' in generic parameter list, "
                           "got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::COMMA,
                      TokenType::GREATER);
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::GREATER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '>' to close generic parameter list, "
                           "got '", stream.peekValue(), "'");
    }

    auto builder = ctx.arena().makeBuilder<GenericParamDeclAST*>(params.size());
    for (GenericParamDeclAST* p : params) builder.push_back(p);
    return builder.build();
}

// ─── parseGenericArgs ───────────────────────────────────────────────────

/// @brief Parse the `<T, U, V>` list at a use site.
///
/// If the current token is not `<`, returns an empty span and consumes
/// nothing. Otherwise consumes the `<`, the argument types, and the `>`.
///
/// Empty `<>` is a syntax error; a generic argument list must have at
/// least one argument.
///
/// The arguments are types. The grammar's `type_arg` production allows
/// either a type or an integer literal; the parser accepts both, and
/// produces a `PrimitiveTypeAST` for the integer form (representing a
/// constant generic argument). Sema validates the count and shape against
/// the declaration's parameters.
ArenaSpan<TypeAST*> parseGenericArgs(TokenStream& stream,
                                     ParserContext& ctx) {
    if (!stream.check(TokenType::LESS)) {
        return ctx.arena().makeBuilder<TypeAST*>().build();
    }
    stream.consume();   // `<`

    if (stream.check(TokenType::GREATER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "a generic argument list must not be empty; "
                           "remove the '<>' or add an argument");
        stream.consume();   // `>`
        return ctx.arena().makeBuilder<TypeAST*>().build();
    }

    std::vector<TypeAST*> args;

    while (!stream.isAtEnd() && !stream.check(TokenType::GREATER) &&
           ctx.canContinue()) {
        // Integer literal as a generic argument (e.g., `Simd<float, 4>`).
        if (stream.check(TokenType::INT_LITERAL)) {
            Token intTok = stream.consume();
            auto* intType = ctx.arena().make<PrimitiveTypeAST>(
                PrimitiveKind::Int);
            intType->loc = intTok.location;
            // Store the lexeme for Sema to read. The parser does not
            // decide whether the integer is a valid generic argument;
            // that's Sema's job.
            args.push_back(intType);
            // The lexeme itself is not stored on PrimitiveTypeAST; a
            // separate AST node would be needed to carry it. See the
            // note at the end of this function.
        } else {
            TypeAST* arg = parseType(stream, ctx);
            if (!arg) {
                ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                                   stream.currentLoc(),
                                   "expected a generic argument type, got '",
                                   stream.peekValue(), "'");
                arg = ctx.arena().make<UnknownTypeAST>();
                arg->loc = stream.currentLoc();
                arg->hasSyntaxError = true;
            }
            args.push_back(arg);
        }

        if (stream.match(TokenType::COMMA)) {
            continue;
        }
        if (stream.check(TokenType::GREATER)) {
            break;
        }

        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or '>' in generic argument list, "
                           "got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::COMMA,
                      TokenType::GREATER);
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::GREATER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '>' to close generic argument list, "
                           "got '", stream.peekValue(), "'");
    }

    auto builder = ctx.arena().makeBuilder<TypeAST*>(args.size());
    for (TypeAST* a : args) builder.push_back(a);
    return builder.build();
}

// =============================================================================
// 9.3 Argument and parameter lists
// =============================================================================

// ─── parseArgList ───────────────────────────────────────────────────────

/// @brief Parse a `(a, b, c)` argument list for a call.
///
/// Consumes the opening and closing parentheses and the arguments. The
/// empty list `()` is legal and produces an empty span.
///
/// The argument list does not permit trailing commas; a `,` before `)`
/// is a syntax error. The comma check happens after each argument.
ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx) {
    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for argument list, got '",
                           stream.peekValue(), "'");
        return ctx.arena().makeBuilder<ExprAST*>().build();
    }

    if (stream.match(TokenType::RPAREN)) {
        return ctx.arena().makeBuilder<ExprAST*>().build();
    }

    std::vector<ExprAST*> args;

    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN) &&
           ctx.canContinue()) {
        ExprAST* arg = parseRequiredExpr(stream, ctx, "argument expression");
        args.push_back(arg);

        if (stream.match(TokenType::COMMA)) {
            // A comma after the last argument is a trailing comma:
            // the next token is `)`. The grammar does not permit this
            // in argument lists.
            if (stream.check(TokenType::RPAREN)) {
                ctx.diag().errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in argument list");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RPAREN)) {
            break;
        }

        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ')' in argument list, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::COMMA,
                      TokenType::RPAREN);
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::RPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ')' to close argument list, got '",
                           stream.peekValue(), "'");
    }

    auto builder = ctx.arena().makeBuilder<ExprAST*>(args.size());
    for (ExprAST* a : args) builder.push_back(a);
    return builder.build();
}

// ─── parseParamList ─────────────────────────────────────────────────────

/// @brief Parse a `(a T, b U)` parameter list.
///
/// `allowNames` controls whether parameters may have names. The leading
/// group of a function declaration or a function literal uses
/// `allowNames = true`; subsequent stages of a curried signature use
/// `allowNames = false` and each parameter is a bare type.
///
/// Variadic parameters (`...T`) are allowed only in the named form; a
/// variadic parameter must be the last in its group. The parser does not
/// enforce the "must be last" rule at parse time — it produces a
/// `ParamAST` with `isVariadic = true` and lets Sema enforce the
/// constraint. This lets the parser accept an argument list with a
/// variadic in the middle and report a targeted error later.
std::vector<ParamAST*> parseParamList(TokenStream& stream,
                                      ParserContext& ctx,
                                      bool allowNames) {
    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' for parameter list, got '",
                           stream.peekValue(), "'");
        return {};
    }

    std::vector<ParamAST*> params;

    if (stream.match(TokenType::RPAREN)) {
        return params;   // empty list
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN) &&
           ctx.canContinue()) {
        ParamAST* param = parseSingleParameter(stream, ctx, allowNames);
        if (param) params.push_back(param);

        if (stream.match(TokenType::COMMA)) {
            // Trailing comma is not permitted.
            if (stream.check(TokenType::RPAREN)) {
                ctx.diag().errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in parameter list");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RPAREN)) {
            break;
        }

        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ')' in parameter list, got '",
                           stream.peekValue(), "'");
        synchronizeTo(stream, ctx,
                      TokenType::COMMA,
                      TokenType::RPAREN);
        if (stream.match(TokenType::COMMA)) continue;
        break;
    }

    if (!stream.match(TokenType::RPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ')' to close parameter list, got '",
                           stream.peekValue(), "'");
    }

    return params;
}

// ─── parseSingleParameter ───────────────────────────────────────────────

/// @brief Parse one parameter inside a parameter list.
///
/// The parameter's form depends on `allowNames`:
///
///   allowNames = true:   `[const] NAME [...] TYPE`
///   allowNames = false:  `TYPE`
///
/// A `const` before the name marks a read-only reference parameter.
/// A `...` before the type marks a variadic parameter. Both modifiers
/// are only meaningful in the named form; a `const` or `...` in the
/// unnamed form is a syntax error.
ParamAST* parseSingleParameter(TokenStream& stream,
                               ParserContext& ctx,
                               bool allowNames) {
    const SourceLocation loc = stream.currentLoc();

    // ─── Named form ───────────────────────────────────────────────────────
    if (allowNames) {
        bool isConstParam = stream.match(TokenType::KW_CONST);

        // Name is required.
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected a parameter name, got '",
                               stream.peekValue(), "'");
            // Recover: produce a placeholder parameter with an
            // UnknownTypeAST type.
            auto* placeholder = ctx.arena().make<ParamAST>(
                ctx.pool().intern(""),
                ctx.arena().make<UnknownTypeAST>(),
                /*isVariadic=*/false,
                isConstParam);
            placeholder->loc = loc;
            placeholder->hasSyntaxError = true;
            return placeholder;
        }
        Token nameTok = stream.consume();
        InternedString name = ctx.pool().intern(nameTok.value);

        // Variadic modifier.
        bool isVariadic = stream.match(TokenType::VARIADIC);

        // Type.
        TypeAST* type = parseType(stream, ctx);
        if (!type) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a type for parameter '",
                               ctx.pool().lookup(name), "', got '",
                               stream.peekValue(), "'");
            type = ctx.arena().make<UnknownTypeAST>();
            type->loc = stream.currentLoc();
            type->hasSyntaxError = true;
        }

        // A variadic parameter's type is `[*]T` in the AST, regardless
        // of how it was written. The grammar's `param` production has
        // `IDENTIFIER '...' type`, which is read as "collect trailing
        // arguments into a `[*]type`". The parser wraps the type in an
        // ArrayTypeAST with the Dynamic kind.
        TypeAST* finalType = type;
        if (isVariadic) {
            auto* arrayType = ctx.arena().make<ArrayTypeAST>(
                ArrayKind::Dynamic, 0, type);
            arrayType->loc = type->loc;
            finalType = arrayType;
        }

        auto* param = ctx.arena().make<ParamAST>(
            name, finalType, isVariadic, isConstParam);
        param->loc = loc;
        if (finalType->hasSyntaxError) param->hasSyntaxError = true;
        return param;
    }

    // ─── Unnamed form ─────────────────────────────────────────────────────
    //
    // A parameter in a subsequent curry stage: just a type.
    TypeAST* type = parseType(stream, ctx);
    if (!type) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                           stream.currentLoc(),
                           "expected a parameter type, got '",
                           stream.peekValue(), "'");
        type = ctx.arena().make<UnknownTypeAST>();
        type->loc = stream.currentLoc();
        type->hasSyntaxError = true;
    }

    auto* param = ctx.arena().make<ParamAST>(
        ctx.pool().intern(""),   // unnamed
        type,
        /*isVariadic=*/false,
        /*isConstParam=*/false);
    param->loc = loc;
    if (type->hasSyntaxError) param->hasSyntaxError = true;
    return param;
}

// =============================================================================
// 9.4 Import paths
// =============================================================================

/// @brief Parse a dotted import path: `a`, `a.b`, `a.b.c`.
///
/// Returns the sequence of identifiers. Does not combine them into a
/// single InternedString; the caller does that.
///
/// The parser does not resolve the path. The path is stored as written
/// and the CLI's linking step matches it against the module table.
std::vector<InternedString> parseImportPath(TokenStream& stream,
                                            ParserContext& ctx) {
    std::vector<InternedString> parts;

    if (!stream.check(TokenType::IDENTIFIER)) {
        return parts;
    }

    while (true) {
        Token part = stream.consume();
        parts.push_back(ctx.pool().intern(part.value));

        if (!stream.match(TokenType::DOT)) {
            break;
        }

        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected an identifier after '.' in import "
                               "path, got '", stream.peekValue(), "'");
            break;
        }
    }

    return parts;
}

// =============================================================================
// 9.5 Trait references
// =============================================================================

/// @brief Parse a comma-separated list of trait names: `A, B, C`.
///
/// Used in a struct's `: Trait1, Trait2` conformance clause and in a
/// trait's `: Parent1, Parent2` inheritance clause. Each entry is a
/// named type; it may carry generic arguments (`Container<int>`).
///
/// The list is not bounded by a delimiter; it ends when the next token
/// is not a comma following a successful ref. The caller is responsible
/// for having consumed the leading `:` and for handling whatever token
/// stops the list (typically `{`).
ArenaSpan<NamedTypeAST*> parseTraitRefList(TokenStream& stream,
                                           ParserContext& ctx) {
    std::vector<NamedTypeAST*> refs;

    while (true) {
        TypeAST* parsed = parseNamedType(stream, ctx);
        if (!parsed || !parsed->isa<NamedTypeAST>()) {
            ctx.diag().errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a trait name, got '",
                               stream.peekValue(), "'");
            // If a comma follows, skip the bad entry and continue.
            if (!stream.match(TokenType::COMMA)) break;
            continue;
        }

        refs.push_back(parsed->as<NamedTypeAST>());

        if (!stream.match(TokenType::COMMA)) {
            break;
        }
    }

    auto builder = ctx.arena().makeBuilder<NamedTypeAST*>(refs.size());
    for (NamedTypeAST* r : refs) builder.push_back(r);
    return builder.build();
}

// =============================================================================
// 9.6 Host target sigils
// =============================================================================

/// @brief Parse `#host(name)`, `#native(name)`, or `#builtin(name)`.
///
/// The caller has established that the current token is `#`; it may have
/// peeked the following identifier to decide the target is a host target
/// or something else. This function consumes the entire sequence:
///
///   `#` IDENTIFIER `(` IDENTIFIER `)`
///
/// The first IDENTIFIER must be `host`, `native`, or `builtin`; anything
/// else is an error. The second IDENTIFIER is the target name.
///
/// On success, `kind` is set to the specific `HostTypeKind` and
/// `targetName` to the interned identifier. Returns true.
///
/// On error, reports a diagnostic and returns false. The caller decides
/// how to recover.
bool parseHostTarget(TokenStream& stream,
                     ParserContext& ctx,
                     HostTypeKind& kind,
                     InternedString& targetName) {
    const SourceLocation loc = stream.currentLoc();

    if (!stream.match(TokenType::HASH)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken, loc,
                           "expected '#', got '", stream.peekValue(), "'");
        return false;
    }

    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a target kind after '#', got '",
                           stream.peekValue(), "'");
        return false;
    }

    Token kindTok = stream.consume();
    std::string_view kindName = ctx.pool().lookupView(
        ctx.pool().intern(kindTok.value));

    if (kindName == "host") {
        kind = HostTypeKind::Host;
    } else if (kindName == "native") {
        kind = HostTypeKind::Native;
    } else if (kindName == "builtin") {
        kind = HostTypeKind::Builtin;
    } else {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           kindTok.location,
                           "expected 'host', 'native', or 'builtin' after "
                           "'#', got '", kindTok.value, "'");
        return false;
    }

    // `(`
    if (!stream.match(TokenType::LPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected '(' after '#", kindTok.value, "', got '",
                           stream.peekValue(), "'");
        return false;
    }

    // Target name.
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedIdentifier,
                           stream.currentLoc(),
                           "expected a target name inside '#",
                           kindTok.value, "(...)', got '",
                           stream.peekValue(), "'");
        return false;
    }
    Token targetTok = stream.consume();
    targetName = ctx.pool().intern(targetTok.value);

    // `)`
    if (!stream.match(TokenType::RPAREN)) {
        ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ')' to close '#", kindTok.value,
                           "(...)', got '", stream.peekValue(), "'");
        return false;
    }

    return true;
}

// =============================================================================
// 9.7 Small shared utilities
// =============================================================================

// ─── Semicolon consumers ────────────────────────────────────────────────

void consumeDeclarationSemicolon(TokenStream& stream,
                                 ParserContext& ctx,
                                 bool required,
                                 const char* declKind) {
    if (stream.match(TokenType::SEMICOLON)) return;
    if (!required) return;

    ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                       stream.currentLoc(),
                       "expected ';' after ", declKind, " declaration");
}

void consumeSubDeclSemicolon(TokenStream& stream,
                             ParserContext& ctx,
                             bool required,
                             const char* declKind) {
    if (stream.match(TokenType::SEMICOLON)) return;
    if (!required) return;

    ctx.diag().errorAt(DiagCode::Syntax_ExpectedToken,
                       stream.currentLoc(),
                       "expected ';' after ", declKind);
}

// ─── makeFuncType ───────────────────────────────────────────────────────

FuncTypeAST* makeFuncType(ParserContext& ctx,
                          std::vector<ParamAST*>&& params,
                          TypeAST* returnType) {
    auto* ft = ctx.arena().make<FuncTypeAST>();
    auto builder = ctx.arena().makeBuilder<ParamAST*>(params.size());
    for (ParamAST* p : params) builder.push_back(p);
    ft->params = builder.build();
    ft->returnType = returnType;
    return ft;
}

// ─── Item-start predicates ──────────────────────────────────────────────

bool startsStructFieldItem(TokenType t) {
    // A struct body item is a field, a const field, or a `@[...]`
    // attribute list preceding one of those. `static` is checked
    // separately by the struct body's list parser, before this
    // predicate is consulted.
    return t == TokenType::IDENTIFIER
        || t == TokenType::KW_CONST
        || t == TokenType::AT_SIGN;
}

bool startsEnumVariantItem(TokenType t) {
    // An enum body item is a variant or a `@[...]` attribute list
    // preceding one.
    return t == TokenType::IDENTIFIER
        || t == TokenType::AT_SIGN;
}

} // namespace lucid::parser