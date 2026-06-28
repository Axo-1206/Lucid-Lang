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
 * - **Comment Skipping**: All peek/advance methods skip LINE_COMMENT,
 *   DOC_COMMENT, and BLOCK_COMMENT tokens automatically.
 * - **Position Management**: Save and restore positions for lookahead and
 *   error recovery.
 * - **Lookahead**: Peek at future tokens without consuming them.
 * - **EOF Handling**: Returns a sentinel EOF token when at the end.
 * 
 * ## Usage Example
 * 
 * ```cpp
 * TokenStream stream(tokens, "example.lucid");
 * 
 * // Check current token
 * if (stream.check(TokenType::IDENTIFIER)) {
 *     Token tok = stream.advance();  // Consumes and skips following comments
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
// Sentinel EOF Token
// =============================================================================

/**
 * @brief Sentinel EOF token returned when the token stream is exhausted.
 * 
 * This token has type `EOF_TOKEN` and empty fields. It is used to avoid
 * null pointer checks when accessing the current token.
 */
const Token TokenStream::eofToken_ = {TokenType::EOF_TOKEN, "", 0, 0};

// =============================================================================
// Construction
// =============================================================================

/**
 * @brief Construct a TokenStream from a vector of tokens.
 * 
 * @param tokens   The vector of tokens to wrap (takes ownership via move).
 * @param filePath The source file path (interned for error reporting).
 * 
 * ## Comments
 * 
 * Comments are NOT stripped from the token vector. Instead, they are skipped
 * transparently by all peek/advance methods. This allows doc comments to be
 * harvested by scanning backward from declarations.
 * 
 * ## Memory Ownership
 * 
 * The TokenStream takes ownership of the token vector via move. The tokens
 * remain in memory for the lifetime of the TokenStream.
 */
TokenStream::TokenStream(std::vector<Token> tokens, InternedString filePath)
    : tokens_(std::move(tokens))
    , filePath_(filePath) {}

// =============================================================================
// Token Consumption
// =============================================================================

/**
 * @brief Return the current token without consuming it.
 * 
 * This method automatically skips comments, returning the first non-comment
 * token at or after the current position. If no non-comment token exists,
 * the sentinel EOF token is returned.
 * 
 * ## Comment Skipping
 * 
 * Comments (LINE_COMMENT, DOC_COMMENT, and BLOCK_COMMENT) are transparently
 * skipped. This means the grammar never sees comments directly. They are
 * harvested separately via `harvestDocComment()`.
 * 
 * ## Performance
 * 
 * This method is O(n) in the worst case (when skipping many comments), but
 * comments are typically few and far between. The `skipCommentsFrom()` method
 * is optimized for sequential access.
 * 
 * @return const Token& The current non-comment token, or EOF token.
 */
const Token& TokenStream::peek() const {
    if (pos_ >= tokens_.size()) {
        return eofToken_;
    }
    // Skip comments from the current position
    size_t next = skipCommentsFrom(pos_);
    if (next >= tokens_.size()) {
        return eofToken_;
    }
    return tokens_[next];
}

/**
 * @brief Consume and return the current token.
 * 
 * This method advances the position past the current token and any following
 * comments. The consumed token is returned.
 * 
 * ## Comment Skipping
 * 
 * After consuming the current token, the position is advanced past any
 * comments that follow. This ensures that the next call to `peek()` or
 * `advance()` will see the next non-comment token.
 * 
 * ## Performance
 * 
 * This method is O(1) plus the cost of skipping comments (O(n) in worst case).
 * 
 * @return Token The consumed token. If at EOF, returns EOF token.
 */
Token TokenStream::advance() {
    if (pos_ >= tokens_.size()) {
        return eofToken_;
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
 * This method peeks at the current token (skipping comments) and compares
 * its type to the given type.
 * 
 * @param type The token type to check against.
 * @return true if the current token is of the given type, false otherwise.
 */
bool TokenStream::check(TokenType type) const {
    return peek().type == type;
}

/**
 * @brief Check if the current token matches any of the given types.
 * 
 * @param types A list of token types to check against.
 * @return true if the current token matches any of the given types.
 */
bool TokenStream::checkAny(std::initializer_list<TokenType> types) const {
    TokenType current = peek().type;
    for (auto t : types) {
        if (current == t) return true;
    }
    return false;
}

/**
 * @brief If the current token matches the given type, consume and return it.
 * 
 * This is a convenient combination of `check()` and `advance()`.
 * 
 * @param type The token type to match.
 * @return true if the token was matched and consumed, false otherwise.
 */
bool TokenStream::match(TokenType type) {
    if (check(type)) {
        advance();
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
 * ## Error Handling
 * 
 * This method does NOT report errors. It is the caller's responsibility to
 * check the token type before consuming it. If the token type doesn't match,
 * the caller should report an error and handle recovery.
 * 
 * @param type The expected token type.
 * @return Token The consumed token. If at EOF, returns EOF token.
 * 
 * @see check() for verifying token type
 * @see match() for combined check-and-consume
 */
Token TokenStream::consume(TokenType type) {
    if (check(type)) {
        return advance();
    }
    // Error will be reported by caller
    return eofToken_;
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
// Lookahead
// =============================================================================

/**
 * @brief Get the type of the next token (after the current one).
 * 
 * This method skips comments and returns the type of the next non-comment
 * token after the current position.
 * 
 * @return TokenType The type of the next token, or EOF_TOKEN if none.
 */
TokenType TokenStream::peekNextType() const {
    size_t next = skipCommentsFrom(pos_ + 1);
    if (next >= tokens_.size()) return TokenType::EOF_TOKEN;
    return tokens_[next].type;
}

/**
 * @brief Get the next token without consuming it.
 * 
 * @return const Token& The next token (skipping comments), or EOF token.
 */
const Token& TokenStream::peekNext() const {
    size_t next = skipCommentsFrom(pos_ + 1);
    if (next >= tokens_.size()) return eofToken_;
    return tokens_[next];
}

/**
 * @brief Get a token at an offset from the current position.
 * 
 * This method allows arbitrary lookahead without consuming tokens. The
 * offset is relative to the current position, NOT counting comments.
 * 
 * @param offset The number of tokens ahead to peek at.
 * @return const Token& The token at the given offset, or EOF token.
 */
const Token& TokenStream::peekAt(size_t offset) const {
    size_t idx = pos_ + offset;
    if (idx >= tokens_.size()) return eofToken_;
    // Skip comments from this position
    idx = skipCommentsFrom(idx);
    if (idx >= tokens_.size()) return eofToken_;
    return tokens_[idx];
}

/**
 * @brief Check if a token type is a primitive type.
 * 
 * This method identifies token types that represent primitive types:
 * - Boolean: bool
 * - Integers: int8, int16, int32, int64, uint8, uint16, uint32, uint64,
 *   byte, short, int, long, ubyte, ushort, uint, ulong
 * - Floating point: float, double, decimal
 * - Text: string, char
 * 
 * @param type The token type to check.
 * @return true if the token type is a primitive type, false otherwise.
 */
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

/**
 * @brief Skip comments from a given position.
 * 
 * This method advances the position past any LINE_COMMENT, DOC_COMMENT, and
 * BLOCK_COMMENT tokens, returning the index of the first non-comment token.
 * 
 * ## Comment Types
 * 
 * - `LINE_COMMENT`: `-- comment` (line comments)
 * - `DOC_COMMENT`: `/-- ... --/` (doc comments)
 * - `BLOCK_COMMENT`: `/- ... -/` (block comments)
 * 
 * ## Performance
 * 
 * This method is O(n) where n is the number of consecutive comments. Comments
 * are typically few, so this is fast.
 * 
 * @param start The starting position to skip comments from.
 * @return size_t The position of the first non-comment token.
 */
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

/**
 * @brief Convert a token to a SourceLocation.
 * 
 * @param tok The token to convert.
 * @return SourceLocation The location of the token.
 */
SourceLocation TokenStream::locOf(const Token& tok) const {
    return SourceLocation(tok.line, tok.column);
}

} // namespace parser