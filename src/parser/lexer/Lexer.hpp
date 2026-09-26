/**
 * @file Lexer.hpp
 *
 * @responsibility Converts one source file's text into a flat token stream.
 *
 * ─── Design: the lexer is a pure function of the source ───────────────────
 * No state survives the call. The parser's buffered lookahead lives in
 * TokenStream, not here. The lexer's only inputs are the source text and
 * a diagnostic sink; its only output is a vector of tokens.
 *
 * ─── Design: the lexer knows only the keyword set ─────────────────────────
 * The lexer's keyword table is the fixed vocabulary from Tokens.hpp: the
 * declaration keywords, the primitive type names, the statement and
 * sequence keywords, and the operator keywords. Every other name is an
 * IDENTIFIER, resolved by Sema against the module's declarations.
 *
 * Because the primitive type names are keywords (see Tokens.hpp), the
 * lexer recognizes them directly. There is no "is this a type name?"
 * predicate in the lexer; that question never arises — every `int` is
 * `KW_INT`, every `Person` is `IDENTIFIER`.
 *
 * ─── Design: errors go through DiagnosticEngine ───────────────────────────
 * The lexer reports errors at the point they are detected. It does not
 * abort: an unknown character is skipped and lexing continues, so the
 * parser sees as much of the program as possible and the user gets more
 * than one error per compile. The final token is always EOF_TOKEN, even
 * on error.
 *
 * ─── Design: string literals are single tokens ────────────────────────────
 * The grammar has no string interpolation. A `"..."` string is one
 * STRING_LITERAL token with escapes processed and no literal newlines.
 * A `"""..."""` raw string is one RAW_STRING_LITERAL token with no
 * escape processing and no newline restriction.
 *
 * ─── Location convention ──────────────────────────────────────────────────
 * Every token carries the source location of its *first character*. A
 * diagnostic at that location points at the start of the offending
 * token, which is where the reader's eye goes.
 */

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
/// The final token is always EOF_TOKEN, even on error. Errors are
/// reported through `diagnostics`; the return value is never empty.
///
/// The lexer does not intern identifiers or literals. It produces raw
/// lexemes; the parser interns them via `ctx.pool()` when it builds AST
/// nodes.
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