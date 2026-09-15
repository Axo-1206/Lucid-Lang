/**
 * @file LookAhead.cpp
 * @brief Lookahead helper functions for the parser.
 * 
 * These functions peek ahead at the token stream without consuming tokens
 * to determine what syntactic construct we're looking at. They are used
 * by the parser to disambiguate between similar constructs.
 * 
 * ## Design Principles
 * 
 * 1. **Non-consuming**: None of these functions should advance the token stream
 * 2. **Fast**: They should only peek at a few tokens ahead
 * 3. **Conservative**: If uncertain, return false to let the parser try another branch
 * 
 * ## Usage
 * 
 * ```cpp
 * if (looksLikeFuncDecl(stream, ctx)) {
 *     return parseFuncDecl(stream, ctx);
 * } else {
 *     return parseVarDecl(stream, ctx);
 * }
 * ```
 */

#include "../Parser.hpp"
#include "core/Tokens.hpp"

namespace parser {

// =============================================================================
// looksLikeFuncDecl
// =============================================================================

bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx) {
    size_t savedPos = stream.getPos();

    if (!stream.checkAny(TokenType::LET, TokenType::CONST)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume();

    stream.match(TokenType::IDENTIFIER);  // name is optional in error cases

    // Skip generic params
    if (stream.check(TokenType::LESS)) {
        int depth = 0;
        while (!stream.isAtEnd()) {
            TokenType t = stream.peekType();
            if (t == TokenType::LESS) depth++;
            else if (t == TokenType::GREATER) {
                depth--;
                if (depth == 0) { stream.consume(); break; }
            }
            stream.consume();
        }
    }

    bool result = is_function_type_keyword(stream.peekType());
    stream.setPos(savedPos);
    return result;
}

// =============================================================================
// looksLikeAnonFunc
// =============================================================================

bool looksLikeAnonFunc(TokenStream& stream, ParserContext& ctx) {
    size_t savedPos = stream.getPos();
    bool result = false;
    
    // 1. Must start with '('
    if (!stream.check(TokenType::LPAREN)) {
        stream.setPos(savedPos);
        return false;
    }
    
    // 2. Parse at least one parameter group
    bool hasValidParamGroup = false;
    
    while (stream.match(TokenType::LPAREN)) {
        
        // Find matching ')'
        int parenDepth = 1;
        while (!stream.isAtEnd() && parenDepth > 0) {
            if (stream.check(TokenType::LPAREN)) parenDepth++;
            if (stream.check(TokenType::RPAREN)) parenDepth--;
            stream.consume();
        }
        
        if (parenDepth > 0) {
            stream.setPos(savedPos);
            return false;
        }
        
        hasValidParamGroup = true;
        
        // After closing ')', check for operators
        if (stream.isAtEnd()) {
            stream.setPos(savedPos);
            return false;
        }
        
        if (stream.peek().is_operator()) {
            stream.setPos(savedPos);
            return false;
        }
    }
    
    if (!hasValidParamGroup) {
        stream.setPos(savedPos);
        return false;
    }
    
    // 3. Skip optional `->` and return type
    if (stream.match(TokenType::ARROW)) {
        
        if (stream.isAtEnd()) {
            stream.setPos(savedPos);
            return false;
        }
        
        if (stream.peek().is_operator()) {
            stream.setPos(savedPos);
            return false;
        }
        
        // Skip return type until we find '{'
        while (!stream.isAtEnd() && !stream.check(TokenType::LBRACE)) {
            if (stream.peek().is_operator() && !stream.check(TokenType::ARROW)) {
                stream.setPos(savedPos);
                return false;
            }
            
            if (stream.check(TokenType::ARROW)) {
                stream.consume();
                if (stream.isAtEnd() || stream.peek().is_operator()) {
                    stream.setPos(savedPos);
                    return false;
                }
                continue;
            }
            
            if (stream.check(TokenType::LPAREN)) {
                int parenDepth = 1;
                stream.consume();
                while (!stream.isAtEnd() && parenDepth > 0) {
                    if (stream.check(TokenType::LPAREN)) parenDepth++;
                    if (stream.check(TokenType::RPAREN)) parenDepth--;
                    stream.consume();
                }
                continue;
            }
            
            if (stream.check(TokenType::LESS)) {
                int angleDepth = 1;
                stream.consume();
                while (!stream.isAtEnd() && angleDepth > 0) {
                    if (stream.check(TokenType::LESS)) angleDepth++;
                    if (stream.check(TokenType::GREATER)) angleDepth--;
                    stream.consume();
                }
                continue;
            }
            
            if (stream.check(TokenType::LBRACE)) {
                break;
            }
            
            stream.consume();
        }
    }
    
    // 4. Must end with '{'
    if (stream.check(TokenType::LBRACE)) {
        result = true;
    }
    
    stream.setPos(savedPos);
    return result;
}

} // namespace parser