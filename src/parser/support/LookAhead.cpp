/**
 * @file LookAhead.cpp
 * @brief Non-consuming disambiguation for the parser.
 *
 * ─── What this file implements ────────────────────────────────────────────
 *   - looksLikeFuncDecl     `let`/`const` NAME ... fn (...) ... header?
 *   - looksLikeAnonFunc     `fn (...)` ... `{ ... }` at expression position?
 *   - looksLikeSliceStart   `[` starts a slice rather than an index?
 *
 * All three save the stream position on entry, scan forward, and restore
 * the position before returning. None consumes tokens the caller will see.
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
 * This file defines two file-local scanning primitives, `skipBalanced` and
 * `skipOneType`, that the public lookaheads share. They are not declared
 * in Parser.hpp because no other file needs them.
 *
 * ─── Design: every `looksLike*` helper lives here ─────────────────────────
 * The convention is that every function named `looksLike*` is defined in
 * this file. The two general-purpose ones are declared in Parser.hpp
 * (`looksLikeFuncDecl`, `looksLikeAnonFunc`) and the slice-specific one is
 * declared there too (`looksLikeSliceStart`). Moving them all here keeps
 * the disambiguation logic in one place, and makes it obvious where to
 * look when the parser's dispatch needs a new shape question answered.
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
/// Any other token, including a different bracket kind, is consumed
/// without affecting this pair's depth. `( [ ] )` is a balanced
/// LPAREN-RPAREN pair containing a balanced LBRACKET-RBRACKET pair; the
/// inner brackets don't affect the outer depth.
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
///   - Identifier-led types   `Vec2`, `int`, `mod::Type`, `Box<int>`
///   - Modifiers              `T?`, `T!`, `T?!`
///   - Arrays                 `[N]T`, `[*]T`, `[_]T`
///   - Ref                    `&T`
///   - Function types         `fn (...)` (possibly curried)
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

            // Module qualification: `mod::Type`.
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

            // The size slot: an integer literal, `*`, or the identifier
            // `_`. The underscore is not a dedicated token — the lexer
            // emits it as an ordinary IDENTIFIER whose value is "_". The
            // check is by value, matching the parser's convention for
            // value-recognized identifiers.
            if (stream.check(TokenType::INT_LITERAL) ||
                stream.check(TokenType::MUL) ||
                isUnderscoreIdentifier(stream)) {
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
        if (t == TokenType::KW_FN_MARKER) {
            stream.consume();   // `fn`

            while (!stream.isAtEnd()) {
                if (!stream.check(TokenType::LPAREN)) {
                    return consumedAny;
                }
                if (!skipBalanced(stream, TokenType::LPAREN, TokenType::RPAREN)) {
                    return consumedAny;
                }

                // Another `fn` continues the curry chain.
                if (stream.check(TokenType::KW_FN_MARKER)) {
                    stream.consume();
                    continue;
                }

                // An arrow introduces either another stage or the return
                // type.
                if (stream.match(TokenType::ARROW)) {
                    if (stream.check(TokenType::KW_FN_MARKER)) {
                        stream.consume();
                        continue;
                    }
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

bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx) {
    (void)ctx;   // unused; kept for signature symmetry

    const size_t savedPos = stream.getPos();

    // ─── 1. Must start with 'let' or 'const' ─────────────────────────────
    if (!stream.checkAny(TokenType::KW_LET, TokenType::KW_CONST)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume();

    // ─── 2. Optional name ────────────────────────────────────────────────
    stream.match(TokenType::IDENTIFIER);

    // ─── 3. Optional generic parameter list ──────────────────────────────
    if (stream.check(TokenType::LESS)) {
        skipBalanced(stream, TokenType::LESS, TokenType::GREATER);
    }

    // ─── 4. Header must begin with a marker or a bare '(' ────────────────
    //
    // The bare-`(` form is the forgotten-marker recovery case. We accept
    // the shape so the dispatcher routes to `parseFuncDecl`, where the
    // targeted error is produced.
    const bool result =
        stream.check(TokenType::KW_FN_MARKER) ||
        stream.check(TokenType::LPAREN);

    stream.setPos(savedPos);
    return result;
}

// =============================================================================
// looksLikeAnonFunc
// =============================================================================

bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx) {
    (void)ctx;   // unused; kept for signature symmetry

    const size_t savedPos = stream.getPos();

    bool sawFirstStage = false;

    // ─── 1. Walk the header: stages, markers, arrows, return type ────────
    while (!stream.isAtEnd()) {
        // 1a. Optional marker.
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
        if (stream.check(TokenType::KW_FN_MARKER)) {
            continue;
        }
        if (stream.check(TokenType::LPAREN)) {
            continue;
        }
        if (stream.match(TokenType::ARROW)) {
            if (stream.check(TokenType::KW_FN_MARKER)) {
                continue;
            }
            if (!skipOneType(stream)) {
                stream.setPos(savedPos);
                return false;
            }
            break;
        }
        if (stream.check(TokenType::LBRACE)) {
            break;
        }

        stream.setPos(savedPos);
        return false;
    }

    // ─── 2. The header must be followed by a block body ──────────────────
    const bool result = sawFirstStage && stream.check(TokenType::LBRACE);

    stream.setPos(savedPos);
    return result;
}

// =============================================================================
// looksLikeSliceStart
// =============================================================================

/// @brief Determine whether a `[` starts a slice rather than an index.
///
/// A slice's bracket pair contains a top-level `..` or `..<`. An index's
/// does not. The helper walks the tokens inside the bracket pair,
/// tracking bracket depth, and returns true at the first top-level range
/// operator it finds.
///
/// The scan is bounded by the closing `]` of the outer bracket pair. If
/// the pair is malformed (missing the closer), the scan stops at EOF and
/// returns false; the caller reports the malformed bracket.
///
/// The scan does not descend into nested brackets for range detection:
/// `a[b[i..j]]` has a `..` inside an inner `[ ... ]`, at bracket depth 1,
/// so it is not detected as a slice of `a`. Only a `..` at depth 0 of the
/// outer bracket pair counts. That is the correct behavior for
/// distinguishing `a[i..j]` (a slice) from `a[b[i..j]]` (an index into
/// `a` whose index expression happens to contain a slice).
///
/// Precondition: the current token is `[`. The stream position is
/// restored before returning, on every path.
bool looksLikeSliceStart(TokenStream& stream) {
    const size_t savedPos = stream.getPos();

    if (!stream.check(TokenType::LBRACKET)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume();   // `[`

    int bracketDepth = 0;
    while (!stream.isAtEnd()) {
        const TokenType t = stream.peekType();

        if (t == TokenType::LBRACKET) {
            bracketDepth++;
            stream.consume();
            continue;
        }
        if (t == TokenType::RBRACKET) {
            if (bracketDepth == 0) {
                // Reached the closing `]` of the outer bracket pair
                // without seeing a range operator. Not a slice.
                stream.setPos(savedPos);
                return false;
            }
            bracketDepth--;
            stream.consume();
            continue;
        }
        if (bracketDepth == 0 &&
            (t == TokenType::RANGE || t == TokenType::RANGE_EXCLUSIVE)) {
            stream.setPos(savedPos);
            return true;
        }

        stream.consume();
    }

    // Reached EOF without a closing `]` or a range operator.
    stream.setPos(savedPos);
    return false;
}

} // namespace lucid::parser