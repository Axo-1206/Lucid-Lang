/// @file TokenStream.hpp
/// @brief A forward-only view of one source file's token vector, with lookahead.
/// 
/// ─── Per-file ──────────────────────────────────────────────────────────────
/// A TokenStream wraps one file's tokens. It is constructed by the CLI's
/// per-file parse step from the output of the lexer, and consumed by the
/// parser. It outlives a single parse function but not the file.
/// 
/// ─── The tape ──────────────────────────────────────────────────────────────
/// The stream provides forward navigation (consume, match), arbitrary
/// lookahead (peek, peekNext, peekAt), and position save/restore for
/// backtracking (getPos, setPos). The parser uses the save/restore pair
/// only in the two disambiguation sites that need it — distinguishing
/// `Arena::method` from `Arena` followed by `::`, and detecting a slice
/// expression versus an index expression.
/// 
/// ─── Comments ──────────────────────────────────────────────────────────────
/// Line and block comments are dropped by the lexer; the stream never sees
/// them. The one exception is the doc-comment form `/-- ... --/`, which the
/// lexer emits as a DOC_COMMENT token. The stream skips DOC_COMMENT tokens
/// in its peek/consume/match accessors so the parser never accidentally
/// sees one, but it keeps them in the underlying token vector so the
/// doc-comment harvester can scan backward from a declaration's start
/// position and recover the comment that preceded it.
/// 
/// ─── No primitive-type predicate ───────────────────────────────────────────
/// The old stream had an `isPrimitiveTypeToken(TokenType)` method. That
/// predicate has no place in the new design: `int`, `float`, `bool`,
/// `string`, `char`, and their sized variants are ordinary identifiers,
/// resolved by Sema against the core scripts' declarations. The parser
/// cannot distinguish a primitive type name from any other identifier at
/// the token level, and it does not need to.

#pragma once

#include "core/Tokens.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace lucid::parser {

/// @brief A forward-only view of one file's tokens, with lookahead.
///
/// The stream is per-file: one instance per source file. It owns the
/// token vector (moved in at construction) and the current position.
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
    ///
    /// The returned Token's string payload is moved out of the token
    /// vector entry, not copied. After this call, the vector entry's
    /// value is empty; only the returned Token holds the payload.
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
    /// Returns `SourceLocation{1, 1}` (the conventional "start of file")
    /// if the stream is exhausted.
    SourceLocation currentLoc() const;

    /// @brief The location of the most recently consumed token.
    ///
    /// Reads a stored value rather than indexing back into the token
    /// vector, because the position can rest on a skipped doc-comment
    /// rather than the token that was actually consumed.
    SourceLocation previousLoc() const;

    // ─── Lookahead ──────────────────────────────────────────────────────

    /// @brief The type of the current token.
    TokenType peekType() { return peek().type; }

    /// @brief The value of the current token. Returns a copy.
    std::string peekValue() { return peek().value; }

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
    // Used by the two parser sites that need to backtrack: distinguishing
    // `Arena::method` from `Arena` followed by `::`, and distinguishing a
    // slice from an index. Both save the position, consume speculatively,
    // then either commit or restore.

    /// @brief The current position in the underlying token vector.
    ///
    /// This is a raw index into the token vector, including DOC_COMMENT
    /// tokens. Passing it back to `setPos` restores the exact state.
    size_t getPos() const noexcept { return pos_; }

    /// @brief Restore a position previously returned by `getPos`.
    void setPos(size_t pos) noexcept { pos_ = pos; }

    // ─── Underlying storage ─────────────────────────────────────────────
    //
    // Exposed so the doc-comment harvester can scan backward from a
    // declaration's start position. The harvester needs the raw token
    // vector, including the DOC_COMMENT tokens the parser never sees.

    /// @brief The full token vector, comments included.
    const std::vector<Token>& getTokens() const noexcept { return tokens_; }

    /// @brief The token at a raw index, comments included.
    const Token& getTokenAt(size_t idx) const { return tokens_[idx]; }

    /// @brief The number of tokens, comments included.
    size_t getTokenCount() const noexcept { return tokens_.size(); }

private:
    std::vector<Token> tokens_;
    size_t             pos_ = 0;

    /// The sentinel returned when the stream is exhausted. A single
    /// static instance, so `peek()` can return a reference without
    /// allocating a fresh token each time.
    static const Token EOF_TOKEN_SENTINEL;

    /// The location of the most recently consumed token. `previousLoc()`
    /// reads this rather than `tokens_[pos_ - 1]`, because `pos_` walks
    /// forward past any comments trailing the consumed token.
    SourceLocation lastConsumedLoc_{1, 1};

    /// @brief Advance `start` past any DOC_COMMENT tokens.
    ///
    /// Line and block comments are dropped by the lexer and never appear
    /// in `tokens_`. Only DOC_COMMENT survives, and the stream skips it
    /// in every accessor except the raw-vector methods.
    size_t skipCommentsFrom(size_t start) const;
};

} // namespace lucid::parser