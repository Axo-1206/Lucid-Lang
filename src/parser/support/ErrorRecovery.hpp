/**
 * @file ErrorRecovery.hpp
 * @brief Error recovery utilities for the Lucid parser.
 *
 * ─── What this file provides ──────────────────────────────────────────────
 * Synchronization functions that let the parser recover from a syntax
 * error by skipping to a known safe point — the next declaration, the
 * next statement, a specific separator, or a closing bracket.
 *
 * The functions are bracket-aware: they track nested `(...)`, `[...]`,
 * and `{...}` while scanning, so a group's contents are skipped as a
 * unit and the scan does not stop in the middle of a balanced construct.
 *
 * ─── Design: three stop conditions ────────────────────────────────────────
 * A synchronize call stops when one of:
 *
 *   - the caller's predicate matches a token at bracket depth zero
 *     (`SyncResult::Matched`),
 *   - a closing bracket is encountered that has no matching opener on
 *     the scan's own stack (`SyncResult::ForeignCloser`), or
 *   - the stream reaches end-of-input (`SyncResult::ReachedEnd`).
 *
 * A caller that lands on `Matched` may act on the current token. A
 * caller that lands on `ForeignCloser` should treat the current token as
 * belonging to an enclosing construct and give up. A caller that lands
 * on `ReachedEnd` may consult `stream.isAtEnd()` if it needs to
 * distinguish that case.
 */

#pragma once

#include "core/Tokens.hpp"
#include "parser/context/TokenStream.hpp"
#include "parser/context/ParserContext.hpp"

#include <initializer_list>
#include <vector>

namespace lucid::parser {

// =============================================================================
// SyncResult
// =============================================================================

/// @brief Why a synchronization call stopped.
enum class SyncResult {
    /// The predicate matched a token at bracket depth zero. The current
    /// token is one of the caller's own targets; the caller may act on
    /// it directly.
    Matched,

    /// The scan encountered a closing bracket that does not match any
    /// opener on its own stack. The bracket belongs to an enclosing
    /// construct (or the source has a bracket-kind mismatch). The
    /// caller should treat this as "no target found" and recover
    /// upward.
    ForeignCloser,

    /// The scan reached end-of-input without matching the predicate or
    /// hitting a foreign closer. The caller may consult
    /// `stream.isAtEnd()` if it needs to distinguish "ran off the end"
    /// from the other cases.
    ReachedEnd,
};

// =============================================================================
// synchronizeUntil
// =============================================================================

/// @brief Skip tokens until `stopAt` matches at bracket depth zero, or
///        until a foreign closer or end-of-input is reached.
///
/// The scan tracks the bracket kinds `(`, `[`, `{` and their closers.
/// An opener is pushed onto a local stack; a closer pops it if it
/// matches the top, or triggers a `ForeignCloser` if it does not.
///
/// The predicate is only consulted at bracket depth zero. This is what
/// makes the scan bracket-aware: a `;` inside a parenthesized group is
/// skipped, because the scan is inside the group and depth is greater
/// than zero.
///
/// @tparam Predicate  A callable that takes a `TokenType` and returns
///                    bool. Called at depth-zero tokens only.
/// @param stream      The token stream to scan.
/// @param ctx         The parsing context. Unused by the current
///                    implementation; the parameter is present so
///                    future versions can report diagnostics if a
///                    recovery decision needs to.
/// @param stopAt      The predicate that determines when to stop.
/// @return Why the scan stopped.
template<typename Predicate>
SyncResult synchronizeUntil(TokenStream& stream,
                            ParserContext& ctx,
                            Predicate stopAt);

// =============================================================================
// synchronizeTo
// =============================================================================

/// @brief Skip tokens until the current token matches any of the given
///        types at bracket depth zero.
///
/// A convenience wrapper over `synchronizeUntil`. The token-type pack
/// becomes the predicate.
template<typename... StopTokens>
SyncResult synchronizeTo(TokenStream& stream,
                         ParserContext& ctx,
                         StopTokens... stopTokens);

// =============================================================================
// synchronizeToBoundary
// =============================================================================

/// @brief Skip tokens until a declaration keyword, a statement keyword,
///        or one of `extraStops` matches at bracket depth zero.
///
/// Unlike `synchronizeTo`, this function ALWAYS stops at any declaration
/// keyword or statement keyword in addition to the caller's extra stop
/// set. That is what makes it safe to use as a general-purpose recovery
/// for a parser that has failed mid-production: it can never skip past
/// the start of the next declaration or statement.
///
/// The two keyword predicates come from `Tokens.hpp`. They cover every
/// token that can begin a declaration or a statement under the current
/// grammar.
///
/// After calling, the caller should inspect `stream.peekType()` (or
/// `stream.check(...)`) to see which token the scan landed on, and use
/// the returned `SyncResult` to distinguish a clean match from a scan
/// that was stopped by a foreign closer or by end-of-input.
///
/// @param extraStops  Construct-specific tokens to also stop at. For a
///                    variable declaration's recovery, for example,
///                    `{ASSIGN, SEMICOLON}`.
SyncResult synchronizeToBoundary(TokenStream& stream,
                                 ParserContext& ctx,
                                 std::initializer_list<TokenType> extraStops = {});

// =============================================================================
// Template implementations
// =============================================================================
//
// `synchronizeUntil` and `synchronizeTo` are templates; their bodies are
// here. `synchronizeToBoundary` is not a template; its body is in
// ErrorRecovery.cpp.

namespace detail {

inline bool isOpenerToken(TokenType t) noexcept {
    return t == TokenType::LPAREN
        || t == TokenType::LBRACKET
        || t == TokenType::LBRACE;
}

inline bool isCloserToken(TokenType t) noexcept {
    return t == TokenType::RPAREN
        || t == TokenType::RBRACKET
        || t == TokenType::RBRACE;
}

inline TokenType matchingCloserFor(TokenType opener) noexcept {
    switch (opener) {
        case TokenType::LPAREN:   return TokenType::RPAREN;
        case TokenType::LBRACKET: return TokenType::RBRACKET;
        case TokenType::LBRACE:   return TokenType::RBRACE;
        default:                  return TokenType::RPAREN;   // unreachable
    }
}

} // namespace detail

template<typename Predicate>
SyncResult synchronizeUntil(TokenStream& stream,
                            ParserContext& ctx,
                            Predicate stopAt) {
    (void)ctx;   // unused by the current implementation

    std::vector<TokenType> expectedClosers;

    while (!stream.isAtEnd()) {
        const TokenType current = stream.peekType();

        if (detail::isCloserToken(current)) {
            if (!expectedClosers.empty() &&
                expectedClosers.back() == current) {
                // A closer that matches the scan's own opener. Pop and
                // continue.
                expectedClosers.pop_back();
                stream.consume();
                continue;
            }
            if (expectedClosers.empty() && stopAt(current)) {
                // The caller's predicate matched a closer at depth
                // zero. That is a clean stop.
                return SyncResult::Matched;
            }
            // A closer that does not match the scan's stack. It
            // belongs to an enclosing construct, or the source has a
            // bracket-kind mismatch. Either way, not ours to consume.
            return SyncResult::ForeignCloser;
        }

        if (expectedClosers.empty() && stopAt(current)) {
            return SyncResult::Matched;
        }

        if (detail::isOpenerToken(current)) {
            expectedClosers.push_back(detail::matchingCloserFor(current));
        }
        stream.consume();
    }

    return SyncResult::ReachedEnd;
}

template<typename... StopTokens>
SyncResult synchronizeTo(TokenStream& stream,
                         ParserContext& ctx,
                         StopTokens... stopTokens) {
    return synchronizeUntil(stream, ctx, [&](TokenType t) {
        return ((t == stopTokens) || ...);
    });
}

} // namespace lucid::parser