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
 * 
 * @design_decision TokenStream uses lexer::tokenize() for lazy lexing
 *   Instead of a separate lexer pass, TokenStream lazily tokenizes
 *   as tokens are consumed. This simplifies the API.
 * 
 * @design_decision Comments are NOT stripped from the token vector
 *   They are skipped transparently by peek/consume methods.
 *   This allows doc comments to be harvested by scanning backward.
 */

#pragma once

#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include <string>
#include <vector>

namespace parser {

/**
 * @brief Wraps a token stream with safe accessors and automatic comment skipping.
 * 
 * TokenStream is per-file - each file gets its own instance.
 * Comments (LINE_COMMENT, DOC_COMMENT, BLOCK_COMMENT) are transparently skipped.
 */
class TokenStream {
public:
    /**
     * @brief Construct a TokenStream from an existing token vector.
     * 
     * @param tokens The token vector (takes ownership via move)
     */
    explicit TokenStream(std::vector<Token> tokens);
    
    // ─── Token Consumption ──────────────────────────────────────────────
    
    /// @brief Return the current token without consuming it.
    const Token& peek();
    
    /// @brief Consume and return the current token (skips following comments).
    Token consume();
    
    /// @brief Check if the current token is of the given type.
    bool check(TokenType type);
    
    /**
     * @brief Check if the current token matches any of the given types.
     */
    template<typename... Types>
    bool checkAny(Types... types) {
        TokenType current = peek().type;
        return ((types == current) || ...);
    }
    
    /**
     * @brief If the current token matches the given type, consume it.
     */
    bool match(TokenType type);
    
    /// @brief Check if we've reached the end of the token stream.
    bool isAtEnd();
    
    /// @brief Get the current source location.
    SourceLocation currentLoc() const;
    
    /// @brief Consume all consecutive tokens of the given type.
    int consumeTrailing(TokenType type);
    
    // ─── Lookahead ──────────────────────────────────────────────────────
    
    TokenType peekType() { return peek().type; }
    std::string peekValue() { return peek().value; }
    TokenType peekNextType();
    const Token& peekNext();
    const Token& peekAt(size_t offset);
    bool isPrimitiveTypeToken(TokenType type) const;
    
    // ─── Position Management ───────────────────────────────────────────
    
    size_t getPos() const { return pos_; }
    void setPos(size_t pos) { pos_ = pos; }
    const std::vector<Token>& getTokens() const { return tokens_; }
    const Token& getTokenAt(size_t idx) const { return tokens_[idx]; }
    size_t getTokenCount() const { return tokens_.size(); }
    
private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    static const Token EOF_TOKEN_SENTINEL;
    
    size_t skipCommentsFrom(size_t start) const;
};

} // namespace parser