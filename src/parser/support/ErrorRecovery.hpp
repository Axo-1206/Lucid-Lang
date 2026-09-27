/**
 * @file ErrorRecovery.hpp
 *
 * @brief Synchronization utilities for the Lucid parser.
 *
 * ─── The recovery model ───────────────────────────────────────────────────
 * Every parser function that can fail has one of two behaviors on error:
 *
 *   1. PARTIAL-PARSE. It reports a diagnostic, produces an AST node with
 *      `hasSyntaxError = true`, and returns that node. Sema skips a node
 *      that carries the flag. This is what the LSP wants: as much
 *      structure as possible, marked.
 *
 *   2. SKIP. It reports a diagnostic and returns `nullptr`. There is no
 *      honest node to produce. The caller that received the `nullptr`
 *      runs a synchronizer to skip to the next safe point and continues.
 *
 * This file provides the synchronizers used by the second case. It does
 * not implement the partial-parse behavior of any specific parser
 * function; that is each parser's own decision, made at its own error
 * sites.
 *
 * ─── What a synchronizer does ─────────────────────────────────────────────
 * A synchronizer consumes tokens until the parser reaches a position
 * where it can plausibly resume. The parser calls one after receiving a
 * `nullptr` from a nested parse, to skip over the broken construct and
 * land on the next thing it knows how to parse.
 *
 * ─── Design: two functions, one scan ──────────────────────────────────────
 * `synchronizeUntil` is the scanning primitive. It takes a predicate and
 * stops when the predicate matches a token at bracket depth zero (or when
 * a foreign closer or end-of-input ends the scan).
 *
 * `synchronizeTo` is a variadic convenience over it: pass TokenTypes, get
 * a predicate that matches any of them.
 *
 * ─── Design: the stop set is the caller's ─────────────────────────────────
 * A synchronizer does not have a built-in "stop at declarations" or
 * "stop at statements" rule. The caller passes a stop set that reflects
 * the construct it is recovering into. This file does not define any
 * stop sets; the vocabulary predicates the caller builds them from
 * (`isDeclarationStart`, `isStatementStart`, ...) live in `Tokens.hpp`
 * because they are facts about the language's token vocabulary, not
 * facts about recovery.
 *
 * The old design had a `synchronizeToBoundary` function that hardcoded a
 * "declaration keyword or statement keyword" rule. That rule conflated
 * two stop sets and got both wrong for the new grammar. The new design
 * makes the stop set explicit at the call site.
 *
 * ─── Design: no ParserContext parameter ───────────────────────────────────
 * The previous version threaded `ParserContext&` through every call. No
 * implementation read it. A synchronizer is called precisely because the
 * caller already reported a diagnostic; a second diagnostic on the same
 * broken construct is noise.
 *
 * ─── Design: bracket-aware scanning ───────────────────────────────────────
 * The scan tracks a local stack of open `(`, `[`, `{`. A stop token is
 * only honored at bracket depth zero. This is what makes `a(foo;bar)`
 * scan past the `;` inside the call.
 *
 * ─── Design: three ways to stop ───────────────────────────────────────────
 *   - `Matched`        — the predicate matched at depth zero. The scan
 *                        stops *on* the token; it does not consume it.
 *   - `ForeignCloser`  — a closer with no matching opener on the scan's
 *                        own stack. The caller should recover upward.
 *   - `ReachedEnd`     — end of input.
 */

#pragma once

#include "core/Tokens.hpp"
#include "parser/context/TokenStream.hpp"

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
    /// construct. The caller should treat this as "no target found in
    /// this construct" and recover upward.
    ForeignCloser,

    /// The scan reached end-of-input.
    ReachedEnd,
};

// =============================================================================
// Internal helpers
// =============================================================================

namespace detail {

/// @brief The closing bracket that matches an opening bracket.
///
/// Precondition: `opener` is `LPAREN`, `LBRACKET`, or `LBRACE`. The
/// default case is unreachable and returns `RPAREN` so the function is
/// total.
inline TokenType matchingCloserFor(TokenType opener) noexcept {
    switch (opener) {
        case TokenType::LPAREN:   return TokenType::RPAREN;
        case TokenType::LBRACKET: return TokenType::RBRACKET;
        case TokenType::LBRACE:   return TokenType::RBRACE;
        default:                  return TokenType::RPAREN;
    }
}

} // namespace detail

// =============================================================================
// synchronizeUntil — the scanning primitive
// =============================================================================

/// @brief Skip tokens until `stopAt` matches at bracket depth zero, or
///        until a foreign closer or end-of-input ends the scan.
///
/// The scan tracks the bracket kinds `(`, `[`, `{` and their closers. An
/// opener is pushed onto a local stack; a closer pops the stack if it
/// matches the top, or triggers `SyncResult::ForeignCloser` if it does
/// not.
///
/// The predicate is consulted only at bracket depth zero.
///
/// The scan stops *on* a matched token; it does not consume it.
///
/// @tparam Predicate  A callable `bool(TokenType)`. Called at depth-zero
///                    tokens only.
/// @param stream      The token stream to scan.
/// @param stopAt      The predicate that determines when to stop.
/// @return Why the scan stopped.
template <typename Predicate>
SyncResult synchronizeUntil(TokenStream& stream, Predicate stopAt) {
    std::vector<TokenType> expectedClosers;

    while (!stream.isAtEnd()) {
        const TokenType current = stream.peekType();

        if (isClosingDelimiter(current)) {
            if (!expectedClosers.empty() &&
                expectedClosers.back() == current) {
                expectedClosers.pop_back();
                stream.consume();
                continue;
            }
            if (expectedClosers.empty() && stopAt(current)) {
                return SyncResult::Matched;
            }
            return SyncResult::ForeignCloser;
        }

        if (expectedClosers.empty() && stopAt(current)) {
            return SyncResult::Matched;
        }

        if (isOpeningDelimiter(current)) {
            expectedClosers.push_back(detail::matchingCloserFor(current));
        }
        stream.consume();
    }

    return SyncResult::ReachedEnd;
}

// =============================================================================
// synchronizeTo — the variadic convenience form
// =============================================================================

/// @brief Skip tokens until the current token matches any of the given
///        types at bracket depth zero.
///
/// A thin wrapper over `synchronizeUntil` that turns the token-type pack
/// into a predicate.
template <typename... StopTokens>
SyncResult synchronizeTo(TokenStream& stream, StopTokens... stopTokens) {
    return synchronizeUntil(stream, [stopTokens...](TokenType t) {
        return ((t == stopTokens) || ...);
    });
}

} // namespace lucid::parser