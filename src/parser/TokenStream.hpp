/**
 * @file TokenStream.hpp
 * @brief Token stream abstraction for a single file.
 * 
 * TokenStream is per-file - each file gets its own token stream.
 * It wraps the token vector and provides safe accessors with
 * automatic comment skipping.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include <string>

namespace parser {

/**
 * @brief Wraps a vector of tokens with safe accessors and automatic comment skipping.
 * 
 * TokenStream is per-file - each file gets its own instance.
 * Comments (LINE_COMMENT, DOC_COMMENT, BLOCK_COMMENT) are transparently skipped.
 * 
 * ## Usage Example
 * 
 * ```cpp
 * TokenStream stream(tokens, filePath);
 * if (stream.check(TokenType::IDENTIFIER)) {
 *     Token tok = stream.advance();
 * }
 * stream.consume(TokenType::LBRACE);
 * ```
 */
struct TokenStream {
    TokenStream() = default;
    TokenStream(std::vector<Token> tokens, InternedString filePath);
    
    // ─── Token Consumption ──────────────────────────────────────────────
    
    /** @brief Return the current token without consuming it. */
    const Token& peek() const;
    
    /** @brief Consume and return the current token. */
    Token advance();
    
    /** @brief Check if the current token is of the given type. */
    bool check(TokenType type) const;
    
    /**
    * @brief Check if the current token matches any of the given types.
    * 
    * @tparam Types The token types to check against (variadic)
    * @param types The token types to check against
    * @return true if the current token matches any of the given types
    * 
    * ## Usage Examples
    * 
    * ```cpp
    * // Check against two types
    * if (stream.checkAny(TokenType::LET, TokenType::CONST)) { ... }
    * 
    * // Check against multiple types
    * if (stream.checkAny(TokenType::STRUCT, TokenType::ENUM, TokenType::TRAIT)) { ... }
    * 
    * // Check against a single type
    * if (stream.checkAny(TokenType::IDENTIFIER)) { ... }
    * ```
    */
    template<typename... Types>
    bool checkAny(Types... types) {
        TokenType current = peek().type;
        // Use a fold expression (C++17) or manual expansion (C++11/14)
        return ((types == current) || ...);
    }
    
    /** @brief If the current token matches, consume and return it. */
    bool match(TokenType type);
    
    /** @brief Consume the current token, expecting it to be of the given type. */
    Token consume(TokenType type);
    
    /** @brief Check if we've reached the end of the token stream. */
    bool isAtEnd() const;
    
    /** @brief Get the current source location. */
    SourceLocation currentLoc() const;
    
    /** @brief Get the file path this stream represents. */
    InternedString getFilePath() const { return filePath_; }
    
    // ─── Lookahead ──────────────────────────────────────────────────────
    
    TokenType peekType() const { return peek().type; }     // Get the token type(kind)
    std::string peekValue() const { return peek().value; } // Get the token name(value)
    TokenType peekNextType() const;
    const Token& peekNext() const;
    const Token& peekAt(size_t offset) const;
    bool isPrimitiveTypeToken(TokenType type) const;
    
    // ─── Position Management ───────────────────────────────────────────
    
    size_t getPos() const { return pos_; }
    void setPos(size_t pos) { pos_ = pos; }
    const std::vector<Token>& getTokens() const { return tokens_; }
    const Token& getTokenAt(size_t idx) const { return tokens_[idx]; }
    size_t getTokenCount() const { return tokens_.size(); }
    
    size_t skipCommentsFrom(size_t start) const;
    SourceLocation locOf(const Token& tok) const;
    
private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    InternedString filePath_;
    static const Token eofToken_;
};

} // namespace parser