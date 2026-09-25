/// @file Lexer.hpp
/// 
/// @responsibility Converts one source file's text into a flat token stream.
/// 
/// ─── Design: the lexer is a pure function of the source ───────────────────
/// No state survives the call. The parser's buffered lookahead lives in
/// TokenStream, not here. The lexer's only inputs are the source text and
/// a diagnostic sink; its only output is a vector of tokens.
/// 
/// ─── Design: the lexer knows only the boot set ────────────────────────────
/// The lexer's keyword table contains *only* the boot set — the frames,
/// the content markers, the statement and concurrency keywords, and the
/// literal keywords. Every name the language appears to have beyond that
/// list (int, float, Vec2, Map, toStr, Stringable, ...) is lexed as
/// IDENTIFIER and resolved by Sema against the core scripts. This is the
/// "core script is the grammar" commitment in its most concrete form: the
/// lexer cannot recognize a name the grammar does not have.
/// 
/// ─── Design: errors go through DiagnosticEngine ───────────────────────────
/// The lexer reports errors at the point they are detected. It does not
/// abort: an unknown character is skipped and lexing continues, so the
/// parser sees as much of the program as possible and the user gets more
/// than one error per compile. The final token is always EOF_TOKEN, even
/// on error.
/// 
/// ─── Design: interpolation is tokens, not string content ──────────────────
/// A string literal with an interpolation — `"a \(x + 1) b"` — is not one
/// token. The lexer emits:
/// 
///     STRING_HEAD("a ")  IDENTIFIER(x)  PLUS  INT_LITERAL(1)
///     STRING_MIDDLE(" b")  STRING_END
/// 
/// The interpolation's expression is lexed by the normal lexer path and its
/// tokens carry their own source locations. The parser handles it with the
/// normal expression grammar; there is no sub-parser and no re-lexing. A
/// string with no interpolation emits a STRING_HEAD followed by a
/// STRING_END, which the parser folds into one string value.
/// 
/// ─── Location convention ──────────────────────────────────────────────────
/// Every token carries the source location of its *first character*. A
/// diagnostic at that location points at the start of the offending token,
/// which is where the reader's eye goes.

#pragma once

#include "core/Tokens.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <string_view>
#include <vector>

namespace lucid::lexer {

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Tokenize a source file into a flat token stream.
///
/// The final token is always EOF_TOKEN, even on error. Errors are reported
/// through `diagnostics`; the return value is never empty.
///
/// The lexer does not intern identifiers or literals. It produces raw
/// lexemes; the parser interns them via `ctx.pool()` when it builds AST
/// nodes. This keeps the lexer's dependency on the session to a single
/// reference (the diagnostic engine) rather than three.
std::vector<Token> tokenize(std::string_view source,
                            lucid::diag::DiagnosticEngine& diagnostics);

/// @brief Tokenize a source file, stopping after `max_tokens` tokens.
///
/// Used by tooling that wants a bounded prefix of the stream. The final
/// token is EOF_TOKEN if the source ended before `max_tokens`, or the
/// last token produced otherwise. Never returns an empty vector.
std::vector<Token> tokenize_n(std::string_view source,
                              size_t max_tokens,
                              lucid::diag::DiagnosticEngine& diagnostics);

// ─────────────────────────────────────────────────────────────────────────────
// Character classification
// ─────────────────────────────────────────────────────────────────────────────
//
// Exposed in the header because the parser uses the same definitions when
// it validates identifiers, and because they are the kind of predicate a
// test wants to call directly.

inline bool isIdentifierStart(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool isIdentifierChar(char c) noexcept {
    return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

inline bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

inline bool isHexDigit(char c) noexcept {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool isBinDigit(char c) noexcept {
    return c == '0' || c == '1';
}

inline bool isOctDigit(char c) noexcept {
    return c >= '0' && c <= '7';
}

} // namespace lucid::lexer