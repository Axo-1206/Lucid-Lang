/// @file TokenStream.hpp
///
/// @brief A forward-only view of one source file's token vector, with
///        arbitrary lookahead.
///
/// ─── Per-file ─────────────────────────────────────────────────────────────
/// A TokenStream wraps one file's tokens. It is constructed by the CLI's
/// per-file parse step from the output of the lexer, and consumed by the
/// parser. It outlives a single parse function but not the file.
///
/// ─── The cursor invariant ─────────────────────────────────────────────────
/// The lexer emits DOC_COMMENT tokens alongside the visible tokens. The
/// parser never wants to see a DOC_COMMENT through the cursor; it reads
/// comments through the raw-token-vector accessors when it harvests a
/// doc comment for a declaration.
///
/// The stream enforces one invariant: after any public method returns,
/// `pos_` is on a visible token (or on the EOF sentinel). Every method
/// that reads `pos_` normalizes it first, and `setPos` normalizes its
/// argument before storing. This makes `peekNext()`, `peekNextType()`,
/// and `peekAt(offset)` trivially correct: `pos_ + 1` and `pos_ + offset`
/// are visible tokens by construction, with no additional comment
/// skipping inside those methods.
///
/// The bug the previous version had was that `peekNext()` and
/// `peekNextType()` skipped comments starting from `pos_ + 1` without
/// first normalizing `pos_`, so if `pos_` happened to be on a comment,
/// they skipped one token too far. The normalization-on-entry rule makes
/// that class of bug impossible.
///
/// ─── Comments ─────────────────────────────────────────────────────────────
/// Line and block comments are dropped by the lexer and never appear in
/// the token vector at all. Only DOC_COMMENT survives, and the cursor
/// skips it. The raw vector, DOC_COMMENT tokens included, is exposed for
/// the doc-comment harvester through `getTokens()`, `getTokenAt()`, and
/// `getTokenCount()`.
///
/// ─── The sentinel ─────────────────────────────────────────────────────────
/// When the stream is exhausted, `peek()` and `peekNext()` return a
/// reference to a single static Token whose type is EOF_TOKEN and whose
/// value is an invalid InternedString. Callers check `isAtEnd()` or
/// `peekType() == TokenType::EOF_TOKEN`; they never compare against the
/// sentinel directly.

#pragma once

#include "core/Tokens.hpp"
#include "core/memory/InternedString.hpp"
#include "core/memory/StringPool.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace lucid::parser {

/// @brief A forward-only view of one file's tokens, with lookahead.
class TokenStream {
public:
    // ─── Construction ───────────────────────────────────────────────────

    /// @brief Construct from a file's token vector.
    ///
    /// The vector is moved in. The final token must be an EOF_TOKEN; the
    /// lexer guarantees this. The stream never appends to the vector.
    explicit TokenStream(std::vector<Token> tokens);

    TokenStream(const TokenStream&)            = delete;
    TokenStream& operator=(const TokenStream&) = delete;
    TokenStream(TokenStream&&)                 = default;
    TokenStream& operator=(TokenStream&&)      = default;

    // ─── Consumption ────────────────────────────────────────────────────

    /// @brief The current token. Does not advance.
    ///
    /// Returns a reference to the EOF sentinel if the stream is exhausted.
    /// The reference is valid until the next call that consumes a token.
    const Token& peek();

    /// @brief Consume and return the current token.
    Token consume();

    /// @brief True if the current token has the given type.
    bool check(TokenType type);

    /// @brief True if the current token has any of the given types.
    template <typename... Types>
    bool checkAny(Types... types) {
        const TokenType current = peek().type;
        return ((types == current) || ...);
    }

    /// @brief If the current token has the given type, consume it and
    ///        return true. Otherwise leave the stream unchanged and
    ///        return false.
    bool match(TokenType type);

    /// @brief True if the stream is at end-of-input.
    bool isAtEnd();

    /// @brief Consume every consecutive token of the given type.
    /// @return The number consumed.
    int consumeTrailing(TokenType type);

    // ─── Location ───────────────────────────────────────────────────────

    /// @brief The location of the current token.
    ///
    /// Returns `SourceLocation{1, 1}` if the stream is exhausted.
    SourceLocation currentLoc();

    /// @brief The location of the most recently consumed token.
    SourceLocation previousLoc() const { return lastConsumedLoc_; }

    // ─── Lookahead ──────────────────────────────────────────────────────

    /// @brief The type of the current token.
    TokenType peekType() { return peek().type; }

    /// @brief The current token's value handle.
    InternedString peekValue() { return peek().value; }

    /// @brief The current token's value as a view into `pool`.
    ///
    /// The caller supplies the pool; the stream holds no reference to it.
    /// Returns an empty view for the EOF sentinel.
    std::string_view peekValueView(const StringPool& pool) {
        return pool.lookupView(peek().value);
    }

    /// @brief The type of the token after the current one.
    TokenType peekNextType();

    /// @brief The token after the current one.
    const Token& peekNext();

    /// @brief The token at `offset` visible tokens past the current one.
    ///
    /// `offset` counts visible (non-comment) tokens: `peekAt(0)` is the
    /// current token, `peekAt(1)` is the same as `peekNext()`, and so on.
    const Token& peekAt(size_t offset);

    // ─── Position save / restore ────────────────────────────────────────
    //
    // Used by the parser's lookahead helpers, which save the position,
    // scan speculatively, and restore.

    /// @brief The current position, normalized to a visible token.
    ///
    /// Passing the result back to `setPos` restores the exact state.
    size_t getPos();

    /// @brief Restore a position previously returned by `getPos`.
    ///
    /// The argument is normalized: if it points at a DOC_COMMENT, the
    /// cursor advances to the next visible token. This makes the
    /// round-trip `setPos(getPos())` exact and makes it safe for a
    /// caller to pass an unnormalized position.
    void setPos(size_t pos);

    // ─── Underlying storage ─────────────────────────────────────────────
    //
    // Exposed so the doc-comment harvester can scan backward from a
    // declaration's start position. The harvester needs the raw token
    // vector, DOC_COMMENT tokens included.

    /// @brief The full token vector, DOC_COMMENT tokens included.
    const std::vector<Token>& getTokens() const noexcept { return tokens_; }

    /// @brief The token at a raw index, DOC_COMMENT tokens included.
    const Token& getTokenAt(size_t idx) const { return tokens_[idx]; }

    /// @brief The number of tokens, DOC_COMMENT tokens included.
    size_t getTokenCount() const noexcept { return tokens_.size(); }

private:
    std::vector<Token> tokens_;

    /// The cursor. Invariant: after any public method returns, this is
    /// the index of a visible token, or `tokens_.size()` if the stream
    /// is exhausted.
    size_t pos_ = 0;

    /// The location of the most recently consumed token. `previousLoc()`
    /// reads this rather than `tokens_[pos_ - 1]`, because `pos_` walks
    /// forward past comments.
    SourceLocation lastConsumedLoc_{1, 1};

    /// The sentinel returned when the stream is exhausted. A single
    /// static instance, so `peek()` can return a reference without
    /// allocating a fresh token each time.
    static const Token EOF_TOKEN_SENTINEL;

    /// Advance `start` past any DOC_COMMENT tokens.
    size_t skipCommentsFrom(size_t start) const;

    /// The cursor position, normalized to a visible token. Every public
    /// method that reads `pos_` calls this first.
    size_t normalizedPos() const { return skipCommentsFrom(pos_); }
};

} // namespace lucid::parser