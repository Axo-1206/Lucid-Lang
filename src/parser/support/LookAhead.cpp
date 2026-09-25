/**
 * @file LookAhead.cpp
 * @brief Non-consuming disambiguation for the parser.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - looksLikeFuncDecl     `let`/`const` NAME ... fn (...) ... header?
 *   - looksLikeAnonFunc     `fn (...)` ... `{ ... }` at expression position?
 *
 * Both save the stream position on entry, scan forward, and restore the
 * position before returning. Neither consumes tokens the caller will see.
 *
 * ─── Design: shape, not validation ────────────────────────────────────────
 * A lookahead answers the disambiguation question "does this *shape* like
 * construct X?" — it does NOT validate the construct. Malformed input that
 * still has the right shape returns `true`, so the real parser produces a
 * targeted diagnostic with the correct context. Only input that clearly
 * cannot be construct X returns `false`.
 *
 * This is what makes the lookaheads error-recovery-friendly. A function
 * declaration with a missing `=` still shapes like a function declaration
 * (`let f fn(...) -> int`), so `looksLikeFuncDecl` returns `true`, the
 * dispatcher routes to `parseFuncDecl`, and `parseFuncDecl` reports the
 * missing `=` with a specific error. If the lookahead had returned
 * `false`, the dispatcher would route to `parseVarDecl`, which would
 * mis-parse the whole thing as a variable declaration and produce a
 * cascade of unrelated errors.
 *
 * ─── Design: conservative on false ────────────────────────────────────────
 * If a function is unsure, it returns `false` so the caller tries the next
 * branch. Preferring `false` over `true` means a mis-parse shows up as a
 * "wrong construct" error rather than silent mis-dispatch — easier to
 * debug and easier to recover from.
 *
 * ─── Local helpers ────────────────────────────────────────────────────────
 * This file defines two file-local helpers, `skipBalanced` and
 * `skipOneType`. They are the scanning primitives both lookaheads use.
 * They are not declared in Parser.hpp because no other file needs them.
 */

#include "parser/Parser.hpp"
#include "core/Tokens.hpp"

namespace lucid::parser {

namespace {

// =============================================================================
// Local scanning primitives
// =============================================================================

/// @brief Skip a balanced pair of brackets starting at the current token.
///
/// On entry, the current token must be `open`. On success, the current
/// token is the one immediately after the matching `close`. On failure
/// (EOF before the close), the stream position is unspecified — the
/// caller must restore it.
///
/// The scan tracks depth so that nested pairs of the same kind are
/// skipped correctly: `( ( ) )` ends after the second `)`, and
/// `( ( )` fails at EOF.
///
/// @param stream The token stream.
/// @param open   The opening bracket token type (LPAREN, LBRACKET, LESS).
/// @param close  The matching closing token type.
/// @return true on success, false on EOF before the close.
bool skipBalanced(TokenStream& stream,
                  TokenType open,
                  TokenType close) {
    if (!stream.check(open)) return false;

    int depth = 0;
    while (!stream.isAtEnd()) {
        const TokenType t = stream.peekType();

        if (t == open) {
            depth++;
            stream.consume();
            continue;
        }
        if (t == close) {
            depth--;
            stream.consume();
            if (depth == 0) return true;
            continue;
        }
        // Any other token, including another bracket kind, is consumed
        // without affecting this pair's depth. `( [ ] )` is a balanced
        // LPAREN-RPAREN pair containing a balanced LBRACKET-RBRACKET
        // pair; the inner brackets don't affect the outer depth.
        stream.consume();
    }
    return false;
}

/// @brief Skip one type in the token stream, for lookahead purposes.
///
/// This is a *shape* skip, not a full type parse. It recognizes the token
/// sequences that can appear in a type and stops at the first token that
/// cannot continue the type. It does NOT validate the type: `Vec2<int`
/// (unclosed), `fn (int) ->` (missing return), `[N]T` with a bad `N`, and
/// every other malformed type pass through. The real parser produces the
/// structural errors.
///
/// Recognized shapes:
///   - Primitive type names   `int`, `float`, ... (as identifiers)
///   - Named types            `Vec2`, `mod::Type`, `Box<int>`
///   - Modifiers              `T?`, `T!`, `T?!`
///   - Arrays                 `[N]T`, `[*]T`, `[_]T`
///   - Ref                    `&T`
///   - Function types         `fn (...)` (possibly curried)
///
/// The `mod::Type` shape uses `DOUBLE_COLON` in the current grammar. The
/// lookahead accepts either `::` (the current spelling) or `:` (an older
/// spelling that should not appear, but is cheap to tolerate).
///
/// @param stream The token stream.
/// @return true if at least one type token was consumed, false otherwise.
bool skipOneType(TokenStream& stream) {
    bool consumedAny = false;

    while (!stream.isAtEnd()) {
        const TokenType t = stream.peekType();

        // ─── Identifier: named type, possibly qualified or generic ──────
        //
        // Under the clean-model design, primitive type names (`int`,
        // `float`, `bool`, ...) are ordinary identifiers. The lookahead
        // treats them the same as any other name: consume the identifier,
        // then check for qualification and generic arguments.
        if (t == TokenType::IDENTIFIER) {
            stream.consume();
            consumedAny = true;

            // Module qualification: `mod::Type`. The `::` is a single
            // token (`DOUBLE_COLON`) in the current grammar.
            if (stream.check(TokenType::DOUBLE_COLON)) {
                stream.consume();   // `::`
                if (stream.check(TokenType::IDENTIFIER)) {
                    stream.consume();
                } else {
                    return consumedAny;   // malformed; stop here
                }
            }

            // Generic arguments: `<...>`
            if (stream.check(TokenType::LESS)) {
                skipBalanced(stream, TokenType::LESS, TokenType::GREATER);
            }

            // Suffixes: `?`, `!`. Order is `?!` only, but the lookahead
            // accepts either order; the parser enforces the rule.
            stream.match(TokenType::QUESTION);
            stream.match(TokenType::BANG);
            return true;
        }

        // ─── Array: `[N]T`, `[*]T`, `[_]T` ──────────────────────────────
        if (t == TokenType::LBRACKET) {
            stream.consume();   // `[`

            // The size slot: an integer literal, `*`, or `_`.
            if (stream.check(TokenType::INT_LITERAL) ||
                stream.check(TokenType::MUL) ||
                stream.check(TokenType::UNDERSCORE)) {
                stream.consume();
            }
            if (!stream.match(TokenType::RBRACKET)) {
                return consumedAny;   // malformed; stop here
            }

            // The element type follows. Recurse to consume it.
            return skipOneType(stream);
        }

        // ─── Reference: `&T` ────────────────────────────────────────────
        if (t == TokenType::BIT_AND) {
            stream.consume();   // `&`
            return skipOneType(stream);
        }

        // ─── Function type: `fn (...)` (possibly curried) ───────────────
        //
        // The grammar has one function-type marker (`fn`) and requires it
        // on every stage. The lookahead walks stages until it finds a
        // non-`fn` token or the end of the type.
        if (t == TokenType::KW_FN_MARKER) {
            stream.consume();   // `fn`

            // A parameter group is required at every stage. Missing the
            // `(` is a shape failure: the lookahead stops here.
            while (!stream.isAtEnd()) {
                if (!stream.check(TokenType::LPAREN)) {
                    return consumedAny;
                }
                if (!skipBalanced(stream, TokenType::LPAREN, TokenType::RPAREN)) {
                    return consumedAny;   // unclosed; stop here
                }

                // After a group, one of:
                //   - another `fn` (adjacent stage in a leading cluster)
                //   - an arrow `->` (curried continuation or return type)
                //   - anything else (void return; the type ends here)
                if (stream.check(TokenType::KW_FN_MARKER)) {
                    stream.consume();
                    continue;
                }

                if (stream.match(TokenType::ARROW)) {
                    if (stream.check(TokenType::KW_FN_MARKER)) {
                        stream.consume();
                        continue;
                    }
                    // The return type is a plain type. Recurse.
                    return skipOneType(stream);
                }

                // Void return: the type ends after the last group.
                return true;
            }
            return consumedAny;
        }

        // Nothing recognizable as a type.
        return consumedAny;
    }

    return consumedAny;
}

} // namespace

// =============================================================================
// looksLikeFuncDecl
// =============================================================================

/// @brief Determine whether the current position begins a function
///        declaration header.
///
/// A function declaration header is:
///
///   ('let' | 'const') IDENTIFIER? generic_params? func_type_chain
///
/// where `func_type_chain` starts with `fn` **or** with a bare `(` — the
/// latter is the forgotten-marker recovery case. `parseFuncDecl` will
/// report the missing marker with full context if the shape matches but
/// the marker is absent.
///
/// Accepting the bare-`(` form here is what lets the dispatcher in
/// `parseDecl` route to `parseFuncDecl` (which produces a targeted
/// diagnostic) instead of `parseVarDecl` (which would mis-parse the whole
/// thing as a variable declaration and produce a cascade).
///
/// This function does NOT validate the header. `let f fn (a int) -> int`
/// (missing `=`) returns `true`; the missing `=` is reported by
/// `parseFuncDecl`. `let f` with nothing after returns `true`; the
/// missing signature is reported by `parseFuncDecl`'s header loop.
///
/// The function restores the stream position before returning. It has no
/// side effects other than the temporary advance-and-restore.
///
/// @param stream The token stream.
/// @param ctx    The parsing context (unused; kept for symmetry).
/// @return true if the current position looks like the start of a
///         function declaration.
bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx) {
    (void)ctx;   // unused; kept for signature symmetry

    const size_t savedPos = stream.getPos();

    // ─── 1. Must start with 'let' or 'const' ─────────────────────────────
    //
    // A function declaration and a variable declaration share the same
    // leading keyword. The dispatcher calls `looksLikeFuncDecl` only
    // after confirming the keyword is present; the check here is
    // defensive and cheap.
    if (!stream.checkAny(TokenType::KW_LET, TokenType::KW_CONST)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume();   // `let` or `const`

    // ─── 2. Optional name ────────────────────────────────────────────────
    //
    // The name may be missing in error-recovery cases (`let fn (...) = ...`
    // with no name). The lookahead accepts the absence; the real parser
    // reports it.
    stream.match(TokenType::IDENTIFIER);

    // ─── 3. Optional generic parameter list ──────────────────────────────
    //
    // `<T>`, `<T : Trait>`, and `<K, V>` all parse as a balanced
    // `<...>`. The lookahead does not validate the contents.
    if (stream.check(TokenType::LESS)) {
        skipBalanced(stream, TokenType::LESS, TokenType::GREATER);
    }

    // ─── 4. Header must begin with a marker or a bare '(' ────────────────
    //
    // The bare-`(` form is the forgotten-marker recovery case: the parser
    // will report "expected 'fn' before parameter group". We accept the
    // shape here so the dispatcher routes to `parseFuncDecl`, where the
    // targeted error is produced.
    //
    // This also matches `let x (1 + 2)` — a variable whose type is
    // missing and whose init is a parenthesized expression. That input
    // is malformed either way; routing it to `parseFuncDecl` gives a
    // clearer "expected 'fn'" diagnostic than routing it to
    // `parseVarDecl` would.
    const bool result =
        stream.check(TokenType::KW_FN_MARKER) ||
        stream.check(TokenType::LPAREN);

    stream.setPos(savedPos);
    return result;
}

// =============================================================================
// looksLikeAnonFunc
// =============================================================================

/// @brief Determine whether the current position begins a function
///        literal.
///
/// A function literal is a func_type chain followed by a block body:
///
///   func_type_chain '{' ... '}'
///
/// The chain starts with `fn` **or** with a bare `(` — the latter is the
/// forgotten-marker recovery case in expression position. Inside an
/// expression, a bare `(` could also be a parenthesized expression; the
/// lookahead has to distinguish them by what comes after the closing `)`.
///
/// This function does NOT validate the header — the same shape-first
/// principle as `looksLikeFuncDecl`. A malformed chain that still *shapes*
/// like a func_type chain (for example, `fn(a int)(b int) -> int { ... }`
/// with the second stage unmarked) returns `true`, so `parseAnonFuncExpr`
/// runs and reports the missing marker with full context.
///
/// It returns `false` only when the input clearly is not an anon func:
///
///   `(a + b)`         — parenthesized expression: after `)` comes `+`,
///                       which cannot continue a header
///   `(a)`             — parenthesized expression: after `)` comes
///                       whatever follows the primary, not a header token
///   `(a int) * b`     — after `)` comes `*`, which cannot continue a header
///   `(a int)` at EOF  — no body follows
///
/// @param stream The token stream.
/// @param ctx    The parsing context (unused; kept for symmetry).
/// @return true if the current position looks like the start of a
///         function literal.
bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx) {
    (void)ctx;   // unused; kept for signature symmetry

    const size_t savedPos = stream.getPos();

    bool sawFirstStage = false;

    // ─── 1. Walk the header: stages, markers, arrows, return type ────────
    //
    // The loop consumes the entire header. It stops when it sees the `{`
    // that introduces the body, or when it sees a token that cannot
    // continue the header.
    while (!stream.isAtEnd()) {
        // 1a. Optional marker.
        //
        // Only the *leading* stage may omit its marker; subsequent stages
        // require one. The lookahead accepts the omission at every stage
        // so that malformed input (a curried literal with a missing
        // marker on an inner stage) still shapes as a function literal
        // and gets a targeted error from the parser.
        if (stream.check(TokenType::KW_FN_MARKER)) {
            stream.consume();
        }

        // 1b. A parameter group is required at every stage.
        if (!stream.check(TokenType::LPAREN)) {
            stream.setPos(savedPos);
            return false;
        }
        if (!skipBalanced(stream, TokenType::LPAREN, TokenType::RPAREN)) {
            stream.setPos(savedPos);
            return false;
        }
        sawFirstStage = true;

        // 1c. What follows this stage?
        //
        //   `fn`    → another stage with a marker; loop
        //   `(`     → another stage with a forgotten marker; loop
        //   `->`    → another stage (if followed by `fn`) or the return
        //             type (if followed by anything else)
        //   `{`     → body; done
        //   other   → not a header; fail
        if (stream.check(TokenType::KW_FN_MARKER)) {
            continue;
        }
        if (stream.check(TokenType::LPAREN)) {
            continue;
        }
        if (stream.match(TokenType::ARROW)) {
            if (stream.check(TokenType::KW_FN_MARKER)) {
                continue;   // arrow-separated stage
            }
            // The return type. `skipOneType` recurses for nested function
            // types, so `fn (int) -> fn (int) -> int` also works.
            if (!skipOneType(stream)) {
                stream.setPos(savedPos);
                return false;
            }
            break;   // after the return type, only `{` or nothing
        }
        if (stream.check(TokenType::LBRACE)) {
            break;   // void return; body follows directly
        }

        // Anything else after a completed stage is not a header.
        stream.setPos(savedPos);
        return false;
    }

    // ─── 2. The header must be followed by a block body ──────────────────
    //
    // If the loop ended at EOF, `stream.check(LBRACE)` is false and the
    // result is false. If it ended at a `{`, the result is true. If it
    // ended at anything else, the loop would have returned false above.
    const bool result = sawFirstStage && stream.check(TokenType::LBRACE);

    stream.setPos(savedPos);
    return result;
}

} // namespace lucid::parser