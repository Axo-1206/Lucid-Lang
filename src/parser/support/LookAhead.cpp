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
#include "debug/DebugUtils.hpp"
#include "core/Tokens.hpp"

namespace parser {

// =============================================================================
// looksLikeFuncDecl
// =============================================================================

bool looksLikeFuncDecl(TokenStream& stream, ParserContext& ctx) {
    size_t savedPos = stream.getPos();

    // 1. Check for `let` or `const`
    if (!stream.checkAny(TokenType::LET, TokenType::CONST)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume(); // Skip let/const

    // 2. The name is optional for THIS check. A function declaration with a
    //    missing name still has to route to parseFuncDecl - parseVarDecl has
    //    no business trying to recover a parameter list or generic list. If
    //    we bailed out here, `const (x int) -> int { ... }` would fall into
    //    parseVarDecl, whose type-probe would "successfully" misparse
    //    `(x int)` as an unnamed function *type* (parseFuncType explicitly
    //    forbids parameter names) and report a confusing, wrong-subsystem
    //    diagnostic instead of the correct "expected function name". So:
    //    skip the identifier if present, but keep looking for the
    //    func-decl markers below either way.
    stream.match(TokenType::IDENTIFIER);

    // 3. Generic parameters are exclusive to function declarations - a
    //    variable declaration's type grammar has no production starting
    //    with '<'. Seeing it here is decisive by itself, even if the
    //    generic list turns out to be malformed - that's parseFuncDecl's
    //    (specifically parseGenericParamDecls') problem to diagnose, not
    //    this lookahead's. No need to balance-match; we're not consuming
    //    the real generic list here, just detecting intent.
    if (stream.check(TokenType::LESS)) {
        stream.setPos(savedPos);
        return true;
    }

    // 4. No generics - the remaining signal is a parameter-list start.
    bool result = stream.check(TokenType::LPAREN);

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
    
    while (stream.check(TokenType::LPAREN)) {
        stream.consume(); // Skip '('
        
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
    if (stream.check(TokenType::ARROW)) {
        stream.consume(); // Skip '->'
        
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

// =============================================================================
// looksLikeStructLiteral
// =============================================================================

bool looksLikeStructLiteral(TokenStream& stream, ParserContext& ctx) {
    size_t savedPos = stream.getPos();
    bool result = false;
    
    // 1. Check for identifier (type name)
    if (!stream.check(TokenType::IDENTIFIER)) {
        stream.setPos(savedPos);
        return false;
    }
    stream.consume(); // Skip identifier
    
    // 2. Check for optional generic arguments
    if (stream.check(TokenType::LESS)) {
        stream.consume(); // Skip '<'
        int angleDepth = 1;
        
        while (!stream.isAtEnd() && angleDepth > 0) {
            if (stream.check(TokenType::LESS)) {
                angleDepth++;
            } else if (stream.check(TokenType::GREATER)) {
                angleDepth--;
                if (angleDepth == 0) {
                    stream.consume(); // Skip '>'
                    break;
                }
            }
            stream.consume();
        }
        
        if (angleDepth > 0) {
            stream.setPos(savedPos);
            return false;
        }
        
        if (stream.check(TokenType::LBRACE)) {
            result = true;
        }
        
        stream.setPos(savedPos);
        return result;
    }
    
    // 3. Check for '{' after identifier (no generic args)
    if (stream.check(TokenType::LBRACE)) {
        result = true;\
    }
    
    stream.setPos(savedPos);
    return result;
}

} // namespace parser