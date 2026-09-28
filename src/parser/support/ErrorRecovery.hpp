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
 * ─── Design: two scans, one primitive ────────────────────────────────────
 * `synchronizeUntil` is the depth-blind scan: its predicate is
 * `bool(TokenType)` and is consulted only at bracket depth zero.
 *
 * `synchronizeUntilDepth` is the depth-aware scan: its predicate is
 * `bool(TokenStream&, int)` and is consulted at every token. Use it when
 * the recovery decision depends on whether the scan is inside a lost
 * block, or when the predicate needs to look ahead (`FN` followed by an
 * identifier is a strong declaration start; a bare `FN` is not).
 *
 * `synchronizeTo` is a variadic convenience over `synchronizeUntil`.
 *
 * The two named stop sets (`isTopLevelRecoveryStop`,
 * `isFunctionDeclRecoveryStop`) live here rather than in each .cpp that
 * needs them, because two translation units share them. A stop set that
 * is used by one caller stays local to that caller.
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
#include "parser/support/GrammarPositions.hpp"

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

/// @brief Skip tokens until `stopAt` matches, passing the current
///        bracket depth and the stream to the predicate.
///
/// Identical scanning to `synchronizeUntil`, with two differences:
///
///   - The predicate is `bool(TokenStream&, int depth)`. The stream is
///     passed so the predicate can look ahead (e.g. to distinguish
///     `FN <ident>` from a bare `FN`); the depth is passed so the
///     predicate can distinguish "a declaration start at the top level"
///     from "a declaration start inside a lost block".
///
///   - The predicate is consulted at every token, not only at depth
///     zero. A caller that wants depth-aware behavior is by definition
///     interested in tokens at depth > 0.
///
/// The scan still stops *on* a matched token, without consuming it.
/// `ForeignCloser` and `ReachedEnd` have the same meanings as in the
/// single-argument form.
///
/// @tparam Predicate  A callable `bool(TokenStream&, int)`.
/// @param stream      The token stream to scan.
/// @param stopAt      The predicate.
/// @return Why the scan stopped.
template <typename Predicate>
SyncResult synchronizeUntilDepth(TokenStream& stream, Predicate stopAt) {
    std::vector<TokenType> expectedClosers;

    while (!stream.isAtEnd()) {
        const TokenType current = stream.peekType();
        const int depth = static_cast<int>(expectedClosers.size());

        if (isClosingDelimiter(current)) {
            if (!expectedClosers.empty() &&
                expectedClosers.back() == current) {
                expectedClosers.pop_back();
                stream.consume();
                continue;
            }
            if (expectedClosers.empty() && stopAt(stream, depth)) {
                return SyncResult::Matched;
            }
            return SyncResult::ForeignCloser;
        }

        if (stopAt(stream, depth)) {
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
// Named stop sets
// =============================================================================
//
// A stop set is a predicate passed to `synchronizeUntilDepth`. Two are
// shared across more than one translation unit, so they live here rather
// than being duplicated in each .cpp that needs them.

/// @brief The stop set for the top-level recovery scan.
///
/// Stops on any token that can begin a declaration at the current brace
/// depth, and on a `;` (a legal empty declaration the caller's loop will
/// skip).
///
/// The depth matters: a strong declaration start is legal only at the
/// top level, so meeting one at depth > 0 means a `}` is missing and the
/// scan should stop so the caller can resume at the declaration. A weak
/// declaration start (`let`, `const`, `@`) is legal inside a body, so it
/// stops the scan only at depth 0.
///
/// The stream is passed so the predicate can do the `FN` lookahead:
/// `FN <ident>` is a strong start, a bare `FN` is not.
inline bool isTopLevelRecoveryStop(TokenStream& stream, int depth) {
    const TokenType current = stream.peekType();

    if (current == TokenType::SEMICOLON) {
        return true;
    }

    if (isStrongDeclarationStart(current, stream.peekNextType())) {
        return true;
    }

    if (depth == 0 && isWeakDeclarationStart(current)) {
        return true;
    }

    return false;
}

/// @brief The stop set for the function-declaration recovery scan.
///
/// Stops on the function body's opener (`{`), the host-target `=`, or
/// any declaration start at the current brace depth. Used by
/// `parseFnDecl` when the parameter list is malformed and the scan is
/// looking for either the body or the next declaration.
///
/// Unlike `isTopLevelRecoveryStop`, this predicate does not stop on `;`:
/// a `;` between a broken `FN` signature and the next declaration is an
/// empty declaration the enclosing loop will skip, not a boundary the
/// scan needs to find.
inline bool isFunctionDeclRecoveryStop(TokenStream& stream, int depth) {
    const TokenType current = stream.peekType();

    if (current == TokenType::LBRACE) return true;
    if (current == TokenType::ASSIGN) return true;

    if (isStrongDeclarationStart(current, stream.peekNextType())) {
        return true;
    }

    if (depth == 0 && isWeakDeclarationStart(current)) {
        return true;
    }

    return false;
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