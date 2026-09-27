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
// SourceLocation reports as "unknown". Its value is an invalid
// InternedString (id 0), which is what a default-constructed handle holds.
const Token TokenStream::EOF_TOKEN_SENTINEL =
    Token{TokenType::EOF_TOKEN, InternedString{}, SourceLocation{}};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

TokenStream::TokenStream(std::vector<Token> tokens)
    : tokens_(std::move(tokens)) {
    // Normalize once at construction. After this, the cursor invariant
    // holds for every public method.
    pos_ = skipCommentsFrom(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Comment skipping
// ─────────────────────────────────────────────────────────────────────────────

size_t TokenStream::skipCommentsFrom(size_t start) const {
    while (start < tokens_.size()) {
        const TokenType type = tokens_[start].type;
        // Only DOC_COMMENT survives to the stream; line and block
        // comments are dropped by the lexer and never appear here.
        if (type == TokenType::DOC_COMMENT) {
            start++;
        } else {
            break;
        }
    }
    return start;
}

// ─────────────────────────────────────────────────────────────────────────────
// Consumption
// ─────────────────────────────────────────────────────────────────────────────

const Token& TokenStream::peek() {
    // normalizedPos() is idempotent and cheap; calling it on every peek
    // makes the cursor invariant self-enforcing rather than relying on
    // every mutating method to maintain it.
    const size_t idx = normalizedPos();
    if (idx >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }
    return tokens_[idx];
}

Token TokenStream::consume() {
    const size_t idx = normalizedPos();
    if (idx >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }

    // Token is trivially copyable (TokenType + InternedString +
    // SourceLocation). Returning by value is a 16-byte copy, not the
    // heap-moving operation the old std::string version performed.
    Token result = tokens_[idx];
    lastConsumedLoc_ = result.location;
    pos_ = skipCommentsFrom(idx + 1);
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
    return normalizedPos() >= tokens_.size();
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

SourceLocation TokenStream::currentLoc() {
    const size_t idx = normalizedPos();
    if (idx < tokens_.size()) {
        return tokens_[idx].location;
    }
    // Start of file. This gives a sensible location for a diagnostic
    // against an exhausted stream.
    return SourceLocation{1, 1};
}

// ─────────────────────────────────────────────────────────────────────────────
// Lookahead
// ─────────────────────────────────────────────────────────────────────────────
//
// Every lookahead method normalizes `pos_` first. With the cursor always
// on a visible token, `pos_ + 1` and `pos_ + offset` are visible tokens
// by construction — no comment skipping needed inside these methods.

const Token& TokenStream::peekNext() {
    const size_t idx = normalizedPos() + 1;
    if (idx >= tokens_.size()) return EOF_TOKEN_SENTINEL;
    return tokens_[idx];
}

TokenType TokenStream::peekNextType() {
    return peekNext().type;
}

const Token& TokenStream::peekAt(size_t offset) {
    const size_t idx = normalizedPos() + offset;
    if (idx >= tokens_.size()) return EOF_TOKEN_SENTINEL;
    return tokens_[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// Position save / restore
// ─────────────────────────────────────────────────────────────────────────────

size_t TokenStream::getPos() {
    // Return a normalized position so the caller can save it and later
    // restore it without worrying about the comment-stripping rule.
    return normalizedPos();
}

void TokenStream::setPos(size_t pos) {
    // Normalize the argument too. This makes setPos(getPos()) exact and
    // makes a raw unnormalized position safe to pass.
    pos_ = skipCommentsFrom(pos);
}

} // namespace lucid::parser