/// @file TokenStream.cpp
/// @brief Implementation of TokenStream.

#include "TokenStream.hpp"

#include <utility>

namespace lucid::parser {

// ─────────────────────────────────────────────────────────────────────────────
// Sentinel
// ─────────────────────────────────────────────────────────────────────────────

// The sentinel is returned by reference when the stream is exhausted. Its
// location is the default-constructed SourceLocation (value 0), which
// SourceLocation reports as "unknown" — a caller that formats a diagnostic
// against it gets "<unknown location>". Its value is empty; a caller that
// wants to name end-of-input checks `isEof()` or `peekType()`.
const Token TokenStream::EOF_TOKEN_SENTINEL =
    Token{TokenType::EOF_TOKEN, std::string{}, SourceLocation{}};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

TokenStream::TokenStream(std::vector<Token> tokens)
    : tokens_(std::move(tokens)) {}

// ─────────────────────────────────────────────────────────────────────────────
// Consumption
// ─────────────────────────────────────────────────────────────────────────────

const Token& TokenStream::peek() {
    // Normalize pos_ as a side effect of reading, not just of consume().
    // Otherwise pos_ can rest on a doc-comment (at file start, or after a
    // lookahead setPos() restore), and the next raw consume() would eat
    // that comment instead of the token peek() just reported.
    pos_ = skipCommentsFrom(pos_);

    if (pos_ >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }
    return tokens_[pos_];
}

Token TokenStream::consume() {
    pos_ = skipCommentsFrom(pos_);

    if (pos_ >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }

    // Move the payload out of the vector entry. The vector entry's value
    // becomes empty; only the returned Token holds it. This avoids the
    // per-token std::string copy that returning by value would otherwise
    // incur.
    Token result = std::move(tokens_[pos_]);
    lastConsumedLoc_ = result.location;
    pos_++;
    pos_ = skipCommentsFrom(pos_);
    return result;
}

bool TokenStream::check(TokenType type) {
    return peek().type == type;
}

bool TokenStream::match(TokenType type) {
    if (check(type)) {
        consume();
        return true;
    }
    return false;
}

bool TokenStream::isAtEnd() {
    pos_ = skipCommentsFrom(pos_);
    return pos_ >= tokens_.size()
        || tokens_[pos_].type == TokenType::EOF_TOKEN;
}

int TokenStream::consumeTrailing(TokenType type) {
    int count = 0;
    while (check(type)) {
        consume();
        count++;
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Location
// ─────────────────────────────────────────────────────────────────────────────

SourceLocation TokenStream::currentLoc() const {
    const size_t p = skipCommentsFrom(pos_);
    if (p < tokens_.size()) {
        return tokens_[p].location;
    }
    // Start of file. This gives a sensible location for a diagnostic
    // against an exhausted stream.
    return SourceLocation{1, 1};
}

SourceLocation TokenStream::previousLoc() const {
    return lastConsumedLoc_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lookahead
// ─────────────────────────────────────────────────────────────────────────────

TokenType TokenStream::peekNextType() {
    const size_t next = skipCommentsFrom(pos_ + 1);
    if (next >= tokens_.size()) return TokenType::EOF_TOKEN;
    return tokens_[next].type;
}

const Token& TokenStream::peekNext() {
    const size_t next = skipCommentsFrom(pos_ + 1);
    if (next >= tokens_.size()) return EOF_TOKEN_SENTINEL;
    return tokens_[next];
}

const Token& TokenStream::peekAt(size_t offset) {
    size_t idx = pos_;
    for (size_t i = 0; i < offset; ++i) {
        idx = skipCommentsFrom(idx);
        if (idx >= tokens_.size()) return EOF_TOKEN_SENTINEL;
        idx++;
    }
    idx = skipCommentsFrom(idx);
    if (idx >= tokens_.size()) return EOF_TOKEN_SENTINEL;
    return tokens_[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// Comment skipping
// ─────────────────────────────────────────────────────────────────────────────

size_t TokenStream::skipCommentsFrom(size_t start) const {
    while (start < tokens_.size()) {
        const TokenType type = tokens_[start].type;
        // Only DOC_COMMENT survives to the stream. Line and block
        // comments are dropped by the lexer and never appear here.
        if (type == TokenType::DOC_COMMENT) {
            start++;
        } else {
            break;
        }
    }
    return start;
}

} // namespace lucid::parser