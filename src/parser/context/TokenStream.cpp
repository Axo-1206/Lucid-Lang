/// @file TokenStream.cpp
/// @brief Implementation of TokenStream.

#include "TokenStream.hpp"
#include "debug/DebugUtils.hpp"

namespace parser {

// ─── Static Sentinel ──────────────────────────────────────────────────────

const Token TokenStream::EOF_TOKEN_SENTINEL = 
    Token{TokenType::EOF_TOKEN, "EOF", 0, 0};

// ─── Construction ───────────────────────────────────────────────────────

TokenStream::TokenStream(std::vector<Token> tokens)
    : tokens_(std::move(tokens)) {}

// ─── Token Consumption ──────────────────────────────────────────────────

const Token& TokenStream::peek() {
    size_t next = skipCommentsFrom(pos_);
    if (next >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }
    return tokens_[next];
}

Token TokenStream::consume() {
    if (pos_ >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }
    Token result = tokens_[pos_];
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
    size_t next = skipCommentsFrom(pos_);
    return next >= tokens_.size() || tokens_[next].type == TokenType::EOF_TOKEN;
}

SourceLocation TokenStream::currentLoc() const {
    if (pos_ < tokens_.size()) {
        return SourceLocation(tokens_[pos_].line, tokens_[pos_].column);
    }
    return SourceLocation(1, 1);
}

// ─── Trailing Token Consumption ────────────────────────────────────────

int TokenStream::consumeTrailing(TokenType type) {
    int count = 0;
    while (check(type)) {
        consume();
        count++;
    }
    return count;
}

// ─── Lookahead ──────────────────────────────────────────────────────────

TokenType TokenStream::peekNextType() {
    size_t next = skipCommentsFrom(pos_ + 1);
    if (next >= tokens_.size()) return TokenType::EOF_TOKEN;
    return tokens_[next].type;
}

const Token& TokenStream::peekNext() {
    size_t next = skipCommentsFrom(pos_ + 1);
    if (next >= tokens_.size()) return EOF_TOKEN_SENTINEL;
    return tokens_[next];
}

const Token& TokenStream::peekAt(size_t offset) {
    size_t idx = pos_ + offset;
    if (idx >= tokens_.size()) return EOF_TOKEN_SENTINEL;
    idx = skipCommentsFrom(idx);
    if (idx >= tokens_.size()) return EOF_TOKEN_SENTINEL;
    return tokens_[idx];
}

bool TokenStream::isPrimitiveTypeToken(TokenType type) const {
    switch (type) {
        case TokenType::TYPE_BOOL:
        case TokenType::TYPE_INT8:
        case TokenType::TYPE_INT16:
        case TokenType::TYPE_INT32:
        case TokenType::TYPE_INT64:
        case TokenType::TYPE_UINT8:
        case TokenType::TYPE_UINT16:
        case TokenType::TYPE_UINT32:
        case TokenType::TYPE_UINT64:
        case TokenType::TYPE_BYTE:
        case TokenType::TYPE_SHORT:
        case TokenType::TYPE_INT:
        case TokenType::TYPE_LONG:
        case TokenType::TYPE_UBYTE:
        case TokenType::TYPE_USHORT:
        case TokenType::TYPE_UINT:
        case TokenType::TYPE_ULONG:
        case TokenType::TYPE_FLOAT:
        case TokenType::TYPE_DOUBLE:
        case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING:
        case TokenType::TYPE_CHAR:
            return true;
        default:
            return false;
    }
}

// ─── Position Management ────────────────────────────────────────────────

size_t TokenStream::skipCommentsFrom(size_t start) const {
    while (start < tokens_.size()) {
        TokenType type = tokens_[start].type;
        if (type == TokenType::LINE_COMMENT || 
            type == TokenType::DOC_COMMENT ||
            type == TokenType::BLOCK_COMMENT) {
            start++;
        } else {
            break;
        }
    }
    return start;
}

} // namespace parser