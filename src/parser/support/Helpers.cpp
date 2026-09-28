/**
 * @file Helpers.cpp
 * @brief Shared utility parsers used across the parser.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - harvestDocComment              scan backward for a doc comment
 *   - parseArgList                   `(a, b, c)`
 *   - parseParamList                 `(a: T, b: U)`
 *   - parseFunctionTypeParamList     `(T, U, V)` (unnamed types)
 *   - parseImportPath                `a.b.c`
 *   - consumeSemicolon               the statement/declaration terminator
 *
 * ─── Design: list parsers own their delimiters ────────────────────────────
 * Each list parser consumes both the opening and closing delimiter of
 * its list. The caller establishes that the opening delimiter is the
 * current token; the list parser consumes it, reads items separated by
 * commas, and consumes the closing delimiter.
 *
 * ─── Design: comma handling ───────────────────────────────────────────────
 * Trailing commas are not permitted in any list in this grammar. A comma
 * followed immediately by the closing delimiter is reported and the list
 * ends.
 *
 * ─── Design: the doc-comment harvester scans backward ─────────────────────
 * harvestDocComment scans *backward* from the current stream position,
 * through the raw token vector, to find the comment that precedes the
 * declaration the caller is about to read. It cannot use the forward
 * cursor because the parser has already consumed past the comments by
 * the time the harvester runs; the comments are visible only through the
 * stream's raw-token-vector accessors.
 */

#include "parser/Parser.hpp"
#include "parser/support/ErrorRecovery.hpp"

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace lucid::diag;

namespace lucid::parser {

// =============================================================================
// harvestDocComment
// =============================================================================

std::optional<DocComment> harvestDocComment(TokenStream& stream,
                                            ParserContext& ctx) {
    const auto& tokens = stream.getTokens();
    const size_t pos = stream.getPos();
    if (pos == 0) return std::nullopt;

    // The declaration's source line. Used to detect "comment on the
    // same line" (trailing form) and "comment on the previous line"
    // (stacked form).
    const uint32_t declLine = stream.peek().location.line();

    std::optional<std::string> trailingText;      // same-line comment
    std::vector<std::string>   stackedLines;      // consecutive above
    uint32_t                   stackedTopLine = 0;
    bool                       hasStackedTopLine = false;
    std::optional<std::string> blockText;         // DOC_COMMENT body

    // Scan backward through the raw token vector. The vector includes
    // the DOC_COMMENT tokens that the forward cursor skips, so this is
    // where they are reachable.
    for (size_t i = pos; i > 0; ) {
        --i;
        const Token& t = tokens[i];

        if (t.type == TokenType::DOC_COMMENT) {
            if (t.location.line() == 0) continue;   // malformed; skip

            // The block form. It must be on the declaration's line or
            // the line immediately above it.
            if (declLine >= t.location.line() &&
                declLine - t.location.line() <= 1) {
                // The body is the token's interned value; the lexer
                // already stripped the `--/` closer and the leading
                // `/--`.
                blockText = std::string(ctx.pool.lookupView(t.value));
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
    // Under the current grammar, only the block form (`/-- ... --/`)
    // survives to the token stream; line comments are dropped by the
    // lexer. The stacked and trailing forms are preserved here as
    // defensive scaffolding for a future grammar that keeps line
    // comments. In today's grammar, they are never populated, so the
    // block branch is the only one that can fire.
    if (blockText.has_value()) {
        return DocComment{ctx.pool.intern(*blockText)};
    }

    if (!stackedLines.empty()) {
        // Join the stacked lines top-to-bottom, matching the source.
        std::string combined;
        for (auto it = stackedLines.rbegin(); it != stackedLines.rend();
             ++it) {
            if (!combined.empty()) combined += '\n';
            combined += *it;
        }
        return DocComment{ctx.pool.intern(combined)};
    }

    if (trailingText.has_value()) {
        return DocComment{ctx.pool.intern(*trailingText)};
    }

    return std::nullopt;
}

// =============================================================================
// consumeSemicolon
// =============================================================================

void consumeSemicolon(TokenStream& stream,
                      ParserContext& ctx,
                      const char* constructKind) {
    if (stream.match(TokenType::SEMICOLON)) return;

    ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                       stream.currentLoc(),
                       "expected ';' after the ", constructKind,
                       ", got '",
                       stream.peekValueView(ctx.pool), "'");
}

// =============================================================================
// parseArgList
// =============================================================================

ArenaSpan<ExprAST*> parseArgList(TokenStream& stream, ParserContext& ctx) {
    // Caller has consumed the opening `(`.
    if (stream.match(TokenType::RPAREN)) {
        return ctx.arena.makeBuilder<ExprAST*>().build();
    }

    std::vector<ExprAST*> args;

    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN) &&
           ctx.canContinue()) {
        ExprAST* arg = parseRequiredExpr(stream, ctx, "argument");
        args.push_back(arg);

        if (stream.match(TokenType::COMMA)) {
            if (stream.check(TokenType::RPAREN)) {
                ctx.diag.errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in argument list");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RPAREN)) break;

        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ')' in argument list, got '",
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
                           "expected ')' to close the argument list, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    auto builder = ctx.arena.makeBuilder<ExprAST*>(args.size());
    for (ExprAST* a : args) builder.push_back(a);
    return builder.build();
}

// =============================================================================
// parseParamList
// =============================================================================

std::vector<ParamAST*> parseParamList(TokenStream& stream,
                                      ParserContext& ctx) {
    std::vector<ParamAST*> params;

    // Caller has consumed the opening `(`.
    if (stream.match(TokenType::RPAREN)) {
        return params;
    }

    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN) &&
           ctx.canContinue()) {
        ParamAST* param = parseParam(stream, ctx);
        if (param != nullptr) {
            params.push_back(param);
        }

        if (stream.match(TokenType::COMMA)) {
            if (stream.check(TokenType::RPAREN)) {
                ctx.diag.errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in parameter list");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RPAREN)) break;

        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ')' in parameter list, got '",
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
                           "expected ')' to close the parameter list, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    return params;
}

// =============================================================================
// parseFunctionTypeParamList
// =============================================================================

ArenaSpan<TypeAST*> parseFunctionTypeParamList(TokenStream& stream,
                                               ParserContext& ctx) {
    // Caller has consumed the opening `(`.
    if (stream.match(TokenType::RPAREN)) {
        return ctx.arena.makeBuilder<TypeAST*>().build();
    }

    std::vector<TypeAST*> params;

    while (!stream.isAtEnd() && !stream.check(TokenType::RPAREN) &&
           ctx.canContinue()) {
        TypeAST* param = parseType(stream, ctx);
        if (param == nullptr) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedType,
                               stream.currentLoc(),
                               "expected a parameter type, got '",
                               stream.peekValueView(ctx.pool), "'");

            param = ctx.arena.make<UnknownTypeAST>();
            param->loc = stream.currentLoc();
            param->hasSyntaxError = true;
        }
        params.push_back(param);

        if (stream.match(TokenType::COMMA)) {
            if (stream.check(TokenType::RPAREN)) {
                ctx.diag.errorAt(DiagCode::Syntax_TrailingComma,
                                   stream.currentLoc(),
                                   "trailing comma in function type's "
                                   "parameter list");
                break;
            }
            continue;
        }
        if (stream.check(TokenType::RPAREN)) break;

        ctx.diag.errorAt(DiagCode::Syntax_ExpectedToken,
                           stream.currentLoc(),
                           "expected ',' or ')' in function type's "
                           "parameter list, got '",
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
                           "expected ')' to close the function type's "
                           "parameter list, got '",
                           stream.peekValueView(ctx.pool), "'");
    }

    auto builder = ctx.arena.makeBuilder<TypeAST*>(params.size());
    for (TypeAST* p : params) builder.push_back(p);
    return builder.build();
}

// =============================================================================
// parseImportPath
// =============================================================================

std::vector<InternedString> parseImportPath(TokenStream& stream,
                                            ParserContext& ctx) {
    std::vector<InternedString> parts;

    if (!stream.check(TokenType::IDENTIFIER)) {
        return parts;
    }

    while (true) {
        Token part = stream.consume();
        parts.push_back(part.value);

        if (!stream.match(TokenType::DOT)) {
            break;
        }

        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diag.errorAt(DiagCode::Syntax_ExpectedIdentifier,
                               stream.currentLoc(),
                               "expected an identifier after '.' in import "
                               "path, got '",
                               stream.peekValueView(ctx.pool), "'");
            break;
        }
    }

    return parts;
}

} // namespace lucid::parser