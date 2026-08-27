/**
 * @file ErrorRecovery.hpp
 * @brief Error recovery utilities for the Lucid parser.
 * 
 * This file provides synchronization functions that allow the parser to
 * recover from syntax errors by skipping to a known safe point (e.g.,
 * the next declaration or statement boundary).
 * 
 * The functions are bracket-aware — they respect nested parentheses,
 * brackets, and braces, so they don't get confused by balanced groups
 * within the skipped region.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "parser/context/TokenStream.hpp"
#include "parser/context/ParserContext.hpp"

#include <initializer_list>

namespace parser {

// =============================================================================
// Error Recovery
// =============================================================================

/**
 * @brief Why a synchronizeUntil()/synchronizeTo()/synchronizeToBoundary()
 *        call stopped where it did.
 *
 * Only Matched vs. ForeignCloser is actually actionable for callers today:
 * Matched means the current token is one of the caller's own targets, safe
 * to act on directly; ForeignCloser means the scan backed off before a
 * closing bracket it doesn't own (an enclosing construct's, or a genuine
 * bracket-kind mismatch in the source) and the caller should treat that as
 * "nothing found, give up" rather than inspecting the current token further.
 * ReachedEnd exists only because the function needs some return value when
 * it runs off the end of the file - if a caller needs to know that
 * specifically, `stream.isAtEnd()` already tells them so directly; no need
 * to further distinguish *why* it ran off the end (e.g. a bracket opened
 * during the scan that never closed) unless a concrete caller ends up
 * needing that.
 */
enum class SyncResult {
    Matched,        // stopAt() fired with no brackets open - clean, expected stop
    ForeignCloser,  // stopped before a closer belonging to an enclosing/mismatched
                    // scope - not ours to consume
    ReachedEnd,     // hit EOF without matching stopAt or hitting a foreign closer
};

/**
 * @brief Skip tokens until a predicate returns true, respecting bracket nesting.
 * 
 * This is the core synchronization primitive. It scans forward through the
 * token stream, tracking bracket depth, and stops when:
 *   - The predicate returns true AND bracket depth is 0 (clean match)
 *   - A closing bracket is encountered that would make bracket depth negative
 *     (foreign closer — a bracket that belongs to an enclosing scope)
 *   - The end of the stream is reached
 *
 * @tparam Predicate A callable that takes a TokenType and returns bool.
 * @param stream The token stream to scan.
 * @param ctx The parsing context (for diagnostics, though this function
 *            does not emit any itself).
 * @param stopAt The predicate that determines when to stop.
 * @return SyncResult indicating why scanning stopped.
 */
template<typename Predicate>
SyncResult synchronizeUntil(TokenStream& stream, ParserContext& ctx, Predicate stopAt);

/**
 * @brief Skip tokens until one of the specified token types is encountered.
 * 
 * Convenience wrapper around synchronizeUntil that stops when the current
 * token matches any of the provided token types.
 *
 * @tparam StopTokens Variadic list of TokenType values to stop at.
 * @param stream The token stream to scan.
 * @param ctx The parsing context.
 * @param stopTokens The token types to stop at.
 * @return SyncResult indicating why scanning stopped.
 */
template<typename... StopTokens>
SyncResult synchronizeTo(TokenStream& stream, ParserContext& ctx, StopTokens... stopTokens);

/**
 * @brief Skip to the nearest declaration/statement boundary, honoring extra
 *        construct-specific stop tokens.
 *
 * Used when a declaration/statement parser fails mid-production (e.g. a variable's
 * type, or a function's return type) and needs to resynchronize without
 * swallowing whatever comes next. Unlike a fixed-token synchronizeTo() call,
 * this ALWAYS also stops at any declaration keyword (is_declaration_keyword)
 * or statement keyword (is_statement_keyword) in addition to `extraStops`,
 * so it can never skip past the start of the next declaration/statement -
 * a bracket-aware skip that only watches a narrow fixed set (e.g. just
 * ASSIGN/SEMICOLON/CONST/LET) will happily consume an entire unrelated
 * `struct { ... }` or `if { ... }` looking for one of its targets, since
 * neither the keyword nor '{' belongs to that set. This function closes
 * that gap.
 *
 * After calling, the caller should inspect stream.peekType() (or
 * stream.check(...)) to see which token it actually landed on, and/or the
 * returned SyncResult to distinguish a clean stop from having been stuck
 * behind an unclosed bracket - this function does not consume the landed-on
 * token and does not report a diagnostic itself.
 *
 * @param extraStops Construct-specific tokens to also stop at (e.g. '=' and
 *        ';' for a variable declaration, or additionally '{' for a function
 *        declaration's body).
 */
SyncResult synchronizeToBoundary(TokenStream& stream, ParserContext& ctx,
                                std::initializer_list<TokenType> extraStops = {});

} // namespace parser
