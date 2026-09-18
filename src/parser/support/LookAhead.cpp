/// @file LookAhead.cpp
/// @brief Lookahead helper functions for the parser.
/// 
/// These functions peek ahead at the token stream without consuming tokens
/// to determine what syntactic construct we're looking at. They are used
/// by the parser to disambiguate between similar constructs.
/// 
/// ## Design Principles
/// 
/// 1. **Non-consuming**: None of these functions should advance the token stream.
/// 2. **Fast**: They should only peek at a few tokens ahead.
/// 3. **Shape, not validation**: A lookahead answers the disambiguation
///    question "does this *shape* like construct X?" — it does NOT validate
///    the construct. Malformed input that still has the right shape returns
///    `true`, so the real parser can produce a targeted diagnostic with the
///    correct context. Only input that clearly cannot be construct X returns
///    `false`. This is what makes them error-recovery-friendly: the
///    forgotten-marker form `(a int)(b int) -> int { ... }` is still an anon
///    func by shape, even though `parseFuncTypeParts` will report the missing
///    `fn`/`cls`.
/// 4. **Conservative on false**: If a function is unsure, it returns `false`
///    so the caller tries the next branch. Preferring `false` over `true`
///    means a mis-parse shows up as a "wrong construct" error rather than
///    silent mis-dispatch — easier to debug and easier to recover from.
/// 
/// ## Usage
/// 
/// ```cpp
/// if (looksLikeFuncDecl(stream, ctx)) {
///     return parseFuncDecl(stream, ctx);
/// } else {
///     return parseVarDecl(stream, ctx);
/// }
/// ```

#include "../Parser.hpp"
#include "core/Tokens.hpp"

namespace parser {

// =============================================================================
// Local Helpers
// =============================================================================
//
// Both lookaheads below need the same two low-level operations:
//
//   - skipBalanced()  — skip a matched pair of brackets
//   - skipOneType()   — skip one type in the token stream
//
// Neither validates. Both are shape-skips: they consume a sequence of
// tokens that *looks like* the thing being skipped, and stop. The real
// parsers produce all structural diagnostics.
//
// They are file-local because no caller outside the two lookaheads needs
// them. If a third lookahead ever needs them, promote them to the header.

/// @brief Skip a balanced pair of brackets starting at the current token.
///
/// On entry, the current token must be `open`. On success, the current
/// token is the one immediately after the matching `close`. On failure
/// (EOF before the close), the stream position is unspecified — the
/// caller must restore it.
///
/// @param stream The token stream.
/// @param open   The opening bracket token type (LPAREN, LBRACKET, LESS).
/// @param close  The matching closing token type.
/// @return true on success, false on EOF before the close.
static bool skipBalanced(TokenStream& stream,
                         TokenType open, TokenType close) {
    if (!stream.check(open)) return false;

    int depth = 0;
    while (!stream.isAtEnd()) {
        TokenType t = stream.peekType();

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
/// This is a *shape* skip, not a full type parse. It recognizes the
/// token sequences that can appear in a type and stops at the first
/// token that cannot. It does NOT validate the type — `Vec2<int`
/// (unclosed), `fn (int) ->` (missing return), `[N]T` with a bad `N`
/// all pass through. The real parser produces any structural errors.
///
/// Recognized shapes:
///   - Primitive keywords      `int`, `float`, `bool`, ...
///   - Named types             `Vec2`, `mod:Type`, `Box<int>`
///   - Modifiers               `T?`, `T!`, `T?!`
///   - Arrays                  `[N]T`, `[*]T`, `[_]T`
///   - Ref / ptr               `&T`, `*T`
///   - Function types          `fn (...)`, `cls (...)...`
///
/// @param stream The token stream.
/// @return true if at least one type token was consumed, false otherwise.
static bool skipOneType(TokenStream& stream) {
    bool consumedAny = false;

    while (!stream.isAtEnd()) {
        TokenType t = stream.peekType();

        // ─── Primitive keyword: `int`, `float`, ... ────────────────────
        if (stream.isPrimitiveTypeToken(t)) {
            stream.consume();
            return true;
        }

        // ─── Identifier: named type, possibly qualified or generic ─────
        if (t == TokenType::IDENTIFIER) {
            stream.consume();
            consumedAny = true;

            // Module qualification: `mod:Type`. Note the lookahead does
            // NOT distinguish `mod:Type` (module-qualified type) from
            // `member:Type` in some other position — it just skips the
            // identifier, the ':', and the identifier. If the real parser
            // needed to know which, it would say so; the lookahead only
            // needs to know "a type shape ends here."
            if (stream.match(TokenType::COLON)) {
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

            // Suffixes: `?`, `!` (in either order; the parser enforces
            // that `!?` is invalid — the lookahead does not).
            stream.match(TokenType::QUESTION);
            stream.match(TokenType::BANG);
            return true;
        }

        // ─── Array: `[N]T`, `[*]T`, `[_]T` ─────────────────────────────
        if (t == TokenType::LBRACKET) {
            stream.consume();

            if (stream.check(TokenType::INT_LITERAL) ||
                stream.check(TokenType::ARRAY_STAR) ||
                stream.check(TokenType::ARRAY_UNDER)) {
                stream.consume();
            }
            if (!stream.match(TokenType::RBRACKET)) {
                return consumedAny;   // malformed; stop here
            }

            // Recurse for the element type.
            return skipOneType(stream);
        }

        // ─── Reference / pointer: `&T`, `*T` ───────────────────────────
        if (t == TokenType::AMPERSAND || t == TokenType::MUL) {
            stream.consume();
            return skipOneType(stream);
        }

        // ─── Function type: `fn (...)...`, `cls (...)...` ──────────────
        if (is_function_type_keyword(t)) {
            stream.consume();
            while (!stream.isAtEnd()) {
                if (!stream.check(TokenType::LPAREN)) return consumedAny;
                if (!skipBalanced(stream, TokenType::LPAREN, TokenType::RPAREN)) {
                    return consumedAny;
                }

                // Another marker → adjacent stage.
                if (is_function_type_keyword(stream.peekType())) {
                    stream.consume();
                    continue;
                }

                // Arrow → either another stage or the return type.
                if (stream.match(TokenType::ARROW)) {
                    if (is_function_type_keyword(stream.peekType())) {
                        stream.consume();
                        continue;
                    }
                    // Return type: recurse so nested function types work.
                    return skipOneType(stream);
                }

                // Void return: stop after the last group.
                return true;
            }
            return consumedAny;
        }

        // Nothing recognizable as a type.
        return consumedAny;
    }

    return consumedAny;
}

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
/// where `func_type_chain` starts with a `fn`/`cls` marker **or** with
/// a bare `(` — the latter is the forgotten-marker recovery case, and
/// `parseFuncTypeParts` will report the missing marker with full context.
/// Accepting the bare-`(` form here is what lets the dispatcher in
/// `parseDecl` route to `parseFuncDecl` (which produces a targeted
/// diagnostic) instead of `parseVarDecl` (which would mis-parse the whole
/// thing as a variable declaration and produce a cascade).
///
/// This function does NOT validate the header. `let f fn (a int) -> int`
/// (missing `=`) returns `true`; the missing `=` is reported by
/// `parseFuncDecl`. `let f` with nothing after returns `true`; the
/// missing signature is reported by `parseFuncTypeParts`.
///
/// @param stream The token stream.
/// @param ctx    The parsing context (unused; kept for symmetry with
///               other lookaheads that may need it in the future).
/// @return true if the current position looks like the start of a
///         function declaration.
bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx) {
    size_t savedPos = stream.getPos();

    // ─── 1. Must start with 'let' or 'const' ─────────────────────────────
    if (!stream.checkAny(TokenType::LET, TokenType::CONST)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume();

    // ─── 2. Optional name (error-recovery: name may be missing) ──────────
    stream.match(TokenType::IDENTIFIER);

    // ─── 3. Optional generic parameter list ──────────────────────────────
    if (stream.check(TokenType::LESS)) {
        skipBalanced(stream, TokenType::LESS, TokenType::GREATER);
    }

    // ─── 4. Header must begin with a marker or a bare '(' ────────────────
    //
    // Bare '(' is accepted as the forgotten-marker recovery case. Note
    // that this also matches `let x (1 + 2)` — a variable whose type is
    // missing and whose init is a parenthesized expression. That input
    // is malformed either way; routing it to `parseFuncDecl` gives a
    // clearer "expected 'fn' or 'cls'" diagnostic than routing it to
    // `parseVarDecl` would.
    bool result = is_function_type_keyword(stream.peekType())
                  || stream.check(TokenType::LPAREN);

    stream.setPos(savedPos);
    return result;
}

// =============================================================================
// looksLikeAnonFunc
// =============================================================================

/// @brief Determine whether the current position begins an anonymous
///        function expression.
///
/// An anonymous function is a func_type chain followed by a block body:
///
///   func_type_chain '{' ... '}'
///
/// The chain can start two ways:
///   - With a `fn`/`cls` marker: `fn (a int) -> int { ... }`
///   - With a bare `(`:             `(a int) -> int { ... }` (names allowed)
///
/// The bare-`(` form is the one used inside expressions like pipeline
/// steps, where the surrounding context already implies "this is a
/// function value."
///
/// This function does NOT validate the header — the same shape-first
/// principle as `looksLikeFuncDecl`. A malformed chain that still *shapes*
/// like a func_type chain (e.g. `(a int)(b int) -> int { ... }`, the
/// forgotten-marker case) returns `true`, so `parseAnonFuncExpr` runs and
/// `parseFuncTypeParts` reports the missing marker with full context.
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
/// @return true if the current position looks like the start of an
///         anonymous function expression.
bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx) {
    size_t savedPos = stream.getPos();

    bool sawFirstStage = false;

    // ─── 1. Skip the header: stages, markers, arrows, return type ────────
    while (!stream.isAtEnd()) {
        // 1a. Optional marker. Every stage may or may not have one; the
        //     real parser enforces that only the first stage may omit it
        //     (the bare-`(` form). The lookahead accepts both forms at
        //     every stage so malformed input still *shapes* correctly.
        if (is_function_type_keyword(stream.peekType())) {
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
        //   marker  → another stage, loop
        //   '('     → another stage with a forgotten marker; the real
        //             parser recovers and diagnoses. Loop so we still
        //             confirm the trailing '{'.
        //   '->'    → another stage or the final return type; loop or
        //             break depending on what follows
        //   '{'     → body; done
        //   other   → not a header; fail
        if (is_function_type_keyword(stream.peekType())) {
            continue;
        }
        if (stream.check(TokenType::LPAREN)) {
            continue;
        }
        if (stream.match(TokenType::ARROW)) {
            if (is_function_type_keyword(stream.peekType())) {
                continue;   // arrow-separated stage
            }
            // Final return type: skip exactly one type, then the header
            // ends. `skipOneType` recurses for nested function types, so
            // `fn (int) -> fn (int) -> int` also works.
            if (!skipOneType(stream)) {
                stream.setPos(savedPos);
                return false;
            }
            break;
        }
        if (stream.check(TokenType::LBRACE)) {
            break;   // void return, body follows directly
        }

        // Anything else after a completed stage is not a header.
        stream.setPos(savedPos);
        return false;
    }

    // ─── 2. The header must be followed by a block body ──────────────────
    bool result = sawFirstStage && stream.check(TokenType::LBRACE);

    stream.setPos(savedPos);
    return result;
}

} // namespace parser