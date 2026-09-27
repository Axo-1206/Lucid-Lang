/**
 * @file Lexer.hpp
 *
 * @responsibility Converts one source file's text into a flat token stream,
 *                 interning every token payload through a StringPool.
 *
 * ─── Design: the lexer interns ────────────────────────────────────────────
 * Every token that carries a payload — an identifier, a keyword, a literal,
 * a doc comment, an operator spelling — stores an InternedString, not a
 * std::string. The lexer calls pool.intern() once per distinct payload and
 * stores the 4-byte handle on the token.
 *
 * This matches the AST's own decision: BaseAST.hpp stores InternedString
 * for every name and literal, so interning in the lexer is just extending
 * that choice one layer up. The parser receives handles and stores them
 * directly; it never calls pool.intern() and never holds a std::string.
 *
 * ─── Design: every token's value is valid ─────────────────────────────────
 * Unlike the alternative "operators carry an empty value" convention, this
 * lexer interns operator spellings too. "+" is interned the first time a
 * PLUS token is produced; every subsequent PLUS token reuses the same ID.
 * The pool cost is O(distinct tokens), not O(tokens), so a large file with
 * a million operators still only interns "+" once.
 *
 * The uniformity buys two things: peekValue() always returns a valid view
 * (no special case for "this token has no spelling"), and Token has one
 * field with one meaning — no "sometimes text, sometimes empty" ambiguity.
 *
 * ─── Design: no conditional keywording ────────────────────────────────────
 * Every keyword in Tokens.hpp is a keyword everywhere. There is no context
 * where "int" is an identifier and no context where "start" is not a
 * keyword. The parser's dispatch is therefore a pure function of the current
 * token type, with no dependence on surrounding context.
 *
 * ─── Design: errors go through DiagnosticEngine ───────────────────────────
 * The lexer reports errors at the point they are detected. It does not
 * abort: an unknown character is skipped, a malformed literal produces an
 * UNKNOWN token, and lexing continues. The parser sees as much of the
 * program as possible and the user gets more than one error per compile.
 * The final token is always EOF_TOKEN, even on error.
 *
 * ─── Location convention ──────────────────────────────────────────────────
 * Every token carries the source location of its *first character*. A
 * diagnostic at that location points at the start of the offending token.
 * An "unterminated string" diagnostic points at the opening quote, not at
 * end-of-file.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/SourceLocation.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <string_view>
#include <vector>

namespace lucid::lexer {

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Tokenize a source file into a flat token stream.
///
/// Every token's `value` field is an InternedString owned by `pool`. The
/// final token is always EOF_TOKEN, even on error.
///
/// The lexer reports errors through `diagnostics`. It does not abort on
/// error; an unrecoverable malformed construct produces an UNKNOWN token
/// and lexing continues, so the parser sees the rest of the file.
std::vector<Token> tokenize(std::string_view source,
                            StringPool& pool,
                            lucid::diag::DiagnosticEngine& diagnostics);

/// @brief Tokenize a source file, stopping after `max_tokens` tokens.
///
/// Used by tooling that wants a bounded prefix of the stream. The final
/// token is EOF_TOKEN if the source ended before `max_tokens`; otherwise
/// the vector ends at the last token produced and does NOT contain an
/// EOF_TOKEN. A caller that feeds the result to TokenStream must either
/// re-tokenize or append an EOF token.
std::vector<Token> tokenize_n(std::string_view source,
                              size_t max_tokens,
                              StringPool& pool,
                              lucid::diag::DiagnosticEngine& diagnostics);

// ─────────────────────────────────────────────────────────────────────────────
// Character classification
// ─────────────────────────────────────────────────────────────────────────────
//
// Exposed because the parser's lookahead helpers and the LSP's token
// highlighter both want them, and duplicating them invites drift.

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