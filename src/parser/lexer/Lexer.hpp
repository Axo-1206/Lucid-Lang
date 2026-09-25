/**
 * @file Lexer.hpp
 *
 * @responsibility Converts source text into a flat token stream.
 *
 * ─── Design: the lexer knows only the boot set ────────────────────────────
 * Lucid's parser recognizes a small fixed vocabulary — the frames, the
 * content markers, the statement keywords, the concurrency keywords, the
 * punctuation, and the literals. Everything else the language appears to
 * have — every primitive type name, every operator's meaning, every trait,
 * every function — is declared in a core script and resolved by Sema
 * against those declarations.
 *
 * The lexer is the first place this matters. Its keyword table contains
 * *only* the boot set. `int`, `float`, `Vec2`, `Map`, `toStr`, `Stringable`
 * and every other name a program uses are lexed as IDENTIFIER and left
 * for Sema. This is the whole point of the "core script is the grammar"
 * commitment: the lexer cannot know a name the grammar does not have.
 *
 * ─── Design: the lexer is a pure function, not a class ────────────────────
 * It takes source text and a diagnostic sink, and returns a vector of
 * tokens. No state survives the call. The parser's buffered lookahead lives
 * in TokenStream, not here.
 *
 * ─── Design: errors go through DiagnosticEngine ───────────────────────────
 * The lexer reports errors at the point they are detected. It does not
 * abort: an unknown character is skipped and lexing continues, so the
 * parser sees as much of the program as possible and the user gets more
 * than one error per compile.
 *
 * ─── Design: interpolation is tokens, not string content ──────────────────
 * A string literal with an interpolation — `"a \(x + 1) b"` — is not one
 * token. The lexer emits:
 *
 *     STRING_HEAD("a ")  IDENTIFIER(x)  PLUS  INT_LITERAL(1)
 *     STRING_MIDDLE(" b")  STRING_END
 *
 * The interpolation's expression is lexed by the normal lexer path and its
 * tokens carry their own source locations. The parser handles it with the
 * normal expression grammar; there is no sub-parser and no re-lexing. A
 * string with no interpolation emits a single STRING_HEAD followed by a
 * STRING_END, which the parser folds into one string value.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <string>
#include <vector>

namespace lucid::lexer {

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Tokenize a source file into a flat token stream.
///
/// The final token is always EOF_TOKEN, even on error. Errors are reported
/// through `diagnostics`; the return value is never empty.
std::vector<Token> tokenize(const std::string& source,
                            diag::DiagnosticEngine& diagnostics);

/// @brief Tokenize a source file, stopping after `max_tokens` tokens.
///
/// Used by tooling that wants a bounded prefix of the stream. The final
/// token is EOF_TOKEN if the source ended before `max_tokens`, or the
/// last token produced otherwise — it is never an empty result.
std::vector<Token> tokenize_n(const std::string& source,
                              size_t max_tokens,
                              diag::DiagnosticEngine& diagnostics);

// ─────────────────────────────────────────────────────────────────────────────
// Character classification
// ─────────────────────────────────────────────────────────────────────────────
//
// Exposed in the header because the parser uses the same definitions when
// it validates identifiers, and because it is the kind of predicate a test
// wants to call directly.

inline bool isIdentifierStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool isIdentifierChar(char c) {
    return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

inline bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

inline bool isHexDigit(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool isBinDigit(char c) {
    return c == '0' || c == '1';
}

inline bool isOctDigit(char c) {
    return c >= '0' && c <= '7';
}

} // namespace lucid::lexer