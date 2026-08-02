/**
 * @file TokenStream.cpp
 * @brief Implementation of TokenStream - safe token consumption with comment skipping.
 * 
 * TokenStream wraps a vector of tokens and provides safe accessors that
 * automatically skip comments. This makes comments invisible to the grammar
 * (they are harvested separately for documentation generation).
 * 
 * ## Key Features
 * 
 * - **Comment Skipping**: All peek/consume methods skip LINE_COMMENT,
 *   DOC_COMMENT, and BLOCK_COMMENT tokens automatically.
 * - **Position Management**: Save and restore positions for lookahead and
 *   error recovery.
 * - **Lookahead**: Peek at future tokens without consuming them.
 * - **EOF Handling**: Returns a sentinel EOF token when at the end.
 * 
 * ## Usage Example
 * 
 * ```cpp
 * TokenStream stream(tokens);
 * 
 * // Check current token
 * if (stream.check(TokenType::IDENTIFIER)) {
 *     Token tok = stream.consume();  // Consumes and skips following comments
 * }
 * 
 * // Lookahead
 * TokenType next = stream.peekNextType();
 * 
 * // Save position for recovery
 * size_t saved = stream.getPos();
 * // ... try parsing ...
 * if (failed) stream.setPos(saved);
 * ```
 */

#include "TokenStream.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace parser {

// =============================================================================
// Construction
// =============================================================================

/**
 * @brief Construct a TokenStream from a vector of tokens.
 * 
 * @param tokens The vector of tokens to wrap (takes ownership via move).
 * 
 * ## Comments
 * 
 * Comments are NOT stripped from the token vector. Instead, they are skipped
 * transparently by all peek/consume methods. This allows doc comments to be
 * harvested by scanning backward from declarations.
 * 
 * ## Memory Ownership
 * 
 * The TokenStream takes ownership of the token vector via move. The tokens
 * remain in memory for the lifetime of the TokenStream.
 * 
 * ## EOF Token
 * 
 * Uses the single EOF_TOKEN_SENTINEL defined in Tokens.hpp, shared with the Lexer.
 */
TokenStream::TokenStream(std::vector<Token> tokens)
    : tokens_(std::move(tokens)) {}

// =============================================================================
// Token Consumption
// =============================================================================

/**
 * @brief Return the current token without consuming it.
 * 
 * This method automatically skips comments, returning the first non-comment
 * token at or after the current position. If no non-comment token exists,
 * the EOF_TOKEN_SENTINEL is returned.
 * 
 * @return const Token& The current non-comment token, or EOF_TOKEN_SENTINEL.
 */
const Token& TokenStream::peek() const {
    if (pos_ >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }
    size_t next = skipCommentsFrom(pos_);
    if (next >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }
    return tokens_[next];
}

/**
 * @brief Consume and return the current token.
 * 
 * This method advances the position past the current token and any following
 * comments. The consumed token is returned.
 * 
 * @return Token The consumed token. If at EOF, returns EOF_TOKEN_SENTINEL.
 */
Token TokenStream::consume() {
    if (pos_ >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }
    Token result = tokens_[pos_];
    pos_++;
    // Skip any comments that follow
    pos_ = skipCommentsFrom(pos_);
    return result;
}

/**
 * @brief Check if the current token is of the given type.
 * 
 * @param type The token type to check against.
 * @return true if the current token is of the given type, false otherwise.
 */
bool TokenStream::check(TokenType type) const {
    return peek().type == type;
}

/**
 * @brief If the current token matches the given type, consume and return it.
 * 
 * @param type The token type to match.
 * @return true if the token was matched and consumed, false otherwise.
 */
bool TokenStream::match(TokenType type) {
    if (check(type)) {
        consume();
        return true;
    }
    return false;
}

/**
 * @brief Consume the current token, expecting it to be of the given type.
 * 
 * This method advances and returns the token without checking its type.
 * The caller is responsible for verifying the type before calling this
 * method, or handling the error case separately.
 * 
 * @param type The expected token type (for documentation, not validated here).
 * @return Token The consumed token. If at EOF, returns EOF_TOKEN_SENTINEL.
 * 
 * @note This method does NOT report errors. It's the caller's responsibility
 *       to check the token type before consuming it.
 * @see check() for verifying token type
 * @see match() for combined check-and-consume
 */
Token TokenStream::consume(TokenType type) {
    // This is a documentation-only parameter - we don't validate here
    // Callers should use check() first or handle errors separately
    if (pos_ >= tokens_.size()) {
        return EOF_TOKEN_SENTINEL;
    }
    Token result = tokens_[pos_];
    pos_++;
    pos_ = skipCommentsFrom(pos_);
    return result;
}

/**
 * @brief Check if the token stream is at the end.
 * 
 * @return true if the current position is past the end of the token vector.
 */
bool TokenStream::isAtEnd() const {
    return pos_ >= tokens_.size() || peek().type == TokenType::EOF_TOKEN;
}

/**
 * @brief Get the current source location.
 * 
 * @return SourceLocation The location of the current token (or EOF location).
 */
SourceLocation TokenStream::currentLoc() const {
    const Token& tok = peek();
    return SourceLocation(tok.line, tok.column);
}

// =============================================================================
// Trailing Token Consumption
// =============================================================================

/**
 * @brief Consume all consecutive tokens of the given type.
 * 
 * This method consumes ALL consecutive tokens of the specified type.
 * It is useful for handling optional trailing tokens like semicolons.
 * 
 * ## Usage
 * 
 * ```cpp
 * // Consume all trailing semicolons
 * int count = stream.consumeTrailing(TokenType::SEMICOLON);
 * ```
 * 
 * @param type The token type to consume.
 * @return The number of tokens consumed (0 if none).
 * 
 * @note This method skips comments before checking for the token.
 */
int TokenStream::consumeTrailing(TokenType type) {
    int count = 0;
    while (check(type)) {
        LOG_PARSER_DETAIL("consumeTrailing: consuming token #", count + 1, 
                          " of type ", debug::tokenTypeToString(type));
        consume();
        count++;
    }
    if (count > 0) {
        LOG_PARSER_DETAIL("consumeTrailing: consumed ", count, 
                          " consecutive tokens of type ", 
                          debug::tokenTypeToString(type));
    }
    return count;
}

// =============================================================================
// Lookahead
// =============================================================================

TokenType TokenStream::peekNextType() const {
    size_t next = skipCommentsFrom(pos_ + 1);
    if (next >= tokens_.size()) return TokenType::EOF_TOKEN;
    return tokens_[next].type;
}

const Token& TokenStream::peekNext() const {
    size_t next = skipCommentsFrom(pos_ + 1);
    if (next >= tokens_.size()) return EOF_TOKEN_SENTINEL;
    return tokens_[next];
}

const Token& TokenStream::peekAt(size_t offset) const {
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

// =============================================================================
// Position Management
// =============================================================================

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

SourceLocation TokenStream::locOf(const Token& tok) const {
    return SourceLocation(tok.line, tok.column);
}

} // namespace parser