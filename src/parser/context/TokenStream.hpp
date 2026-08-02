/**
 * @file TokenStream.hpp
 * @brief Token stream abstraction for a single file.
 * 
 * TokenStream is per-file - each file gets its own token stream.
 * It wraps the token vector and provides safe accessors with
 * automatic comment skipping.
 * 
 * @design_decision TokenStream is a "tape" - it provides forward-only
 *   navigation with lookahead and position save/restore for backtracking.
 *   It does NOT own the file path - that's stored in ModuleAST.
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
 * TokenStream stream(tokens);
 * if (stream.check(TokenType::IDENTIFIER)) {
 *     Token tok = stream.consume();  // Consumes the current token
 * }
 * stream.consume(TokenType::LBRACE);
 * ```
 */
struct TokenStream {
    TokenStream() = default;
    explicit TokenStream(std::vector<Token> tokens);
    
    // ─── Token Consumption ──────────────────────────────────────────────
    
    /// @brief Return the current token without consuming it.
    const Token& peek() const;
    
    /// @brief Consume and return the current token (skips following comments).
    Token consume();
    
    /// @brief Check if the current token is of the given type.
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
    * if (stream.checkAny(TokenType::LET, TokenType::CONST)) { ... }
    * if (stream.checkAny(TokenType::STRUCT, TokenType::ENUM, TokenType::TRAIT)) { ... }
    * ```
    */
    template<typename... Types>
    bool checkAny(Types... types) {
        TokenType current = peek().type;
        return ((types == current) || ...);
    }
    
    /**
     * @brief If the current token matches the given type, consume it.
     * 
     * @param type The token type to match.
     * @return true if the token was matched and consumed, false otherwise.
     */
    bool match(TokenType type);
    
    /**
     * @brief Consume the current token, asserting it's of the given type.
     * 
     * This method consumes the token without checking its type.
     * The caller must verify the type before calling, or handle errors separately.
     * 
     * @param type The expected token type (for documentation, not validated here).
     * @return Token The consumed token, or EOF token if at end.
     */
    Token consume(TokenType type);
    
    /// @brief Check if we've reached the end of the token stream.
    bool isAtEnd() const;
    
    /// @brief Get the current source location.
    SourceLocation currentLoc() const;
    
    /// @brief Consume all consecutive tokens of the given type.
    int consumeTrailing(TokenType type);
    
    // ─── Lookahead ──────────────────────────────────────────────────────
    
    TokenType peekType() const { return peek().type; }
    std::string peekValue() const { return peek().value; }
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
    // filePath_ REMOVED - use ModuleAST::filePath or diagnostic::ScopedSource
};

} // namespace parser