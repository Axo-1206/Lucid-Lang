/**
 * @file ParserAttributes.cpp
 * @brief Attribute parsing for the Luc parser.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the parsing of compiler attributes (e.g., `@inline`,
 * `@deprecated`, `@extern`). Attributes are metadata attached to declarations.
 * 
 * ## Parser Responsibility vs Semantic Responsibility
 * 
 * | Concern                          | Parser | Semantic |
 * |----------------------------------|--------|----------|
 * | Syntax: @ followed by identifier | ✓      |          |
 * | Syntax: parentheses and commas   | ✓      |          |
 * | Valid attribute name?            |        | ✓ (E3001) |
 * | Correct declaration context?     |        | ✓ (E3004) |
 * | Correct argument count?          |        | ✓ (E3002) |
 * | Correct argument types?          |        | ✓ (E3003) |
 * | Mutual exclusion                 |        | ✓ (E3009, E2013) |
 * 
 * The parser only checks syntactic well‑formedness. Attribute name validation
 * is deferred to the semantic phase (AttributeRegistry).
 * 
 * @see AttributeRegistry.cpp for semantic validation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

// ============================================================================
// 1. ATTRIBUTE COLLECTION
// ============================================================================

/**
 * @brief Parses a sequence of attributes preceding a declaration.
 * 
 * Grammar: { '@' IDENTIFIER [ '(' attr_arg_list ')' ] }
 * 
 * @return std::vector<AttributePtr> – may be empty.
 */
std::vector<AttributePtr> Parser::parseAttributes() {
    std::vector<AttributePtr> attrs;
    while (ts_.check(TokenType::AT_SIGN)) {
        AttributePtr attr = parseAttribute();
        if (attr) {
            attrs.push_back(std::move(attr));
        }
        // On parse failure, parseAttribute already advanced the token stream
        // so we don't loop infinitely.
    }
    return attrs;
}

// ============================================================================
// 2. SINGLE ATTRIBUTE PARSER
// ============================================================================

/**
 * @brief Parses a single attribute and its optional arguments.
 * 
 * Grammar: '@' IDENTIFIER [ '(' attr_arg_list ')' ]
 * 
 * @return AttributePtr – parsed node, or nullptr on syntax error.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '@'
 * On exit:  positioned after the attribute (and its arguments, if any)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing attribute name after '@': report error (E1003), return nullptr
 * - Missing ')' after argument list: report error (E1011), recover
 * - Invalid argument literal: parseAttributeArgLiteral() reports error and
 *   advances; the argument is skipped, parsing continues
 */
AttributePtr Parser::parseAttribute() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AT_SIGN, "expected '@'");
    
    // Attribute name is required – syntax error if missing
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E1003, "expected attribute name after '@'");
        return nullptr;
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    
    auto attr = arena_.make<AttributeAST>();
    attr->loc = loc;
    attr->name = name;
    
    // Parse optional argument list
    if (ts_.match(TokenType::LPAREN)) {
        std::vector<AttributeArgPtr> args;
        
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            // Handle commas between arguments
            if (!args.empty() && !ts_.check(TokenType::COMMA)) {
                errorAt(DiagCode::E1002, "expected ',' between attribute arguments");
                break;
            }
            if (ts_.check(TokenType::COMMA)) {
                ts_.advance();
            }
            
            AttributeArgPtr arg = parseAttributeArgLiteral();
            if (arg) {
                args.push_back(std::move(arg));
            } else {
                // Invalid argument – skip it and continue.
                // parseAttributeArgLiteral() already advanced the token.
            }
        }
        
        // Expect closing ')'
        if (!ts_.check(TokenType::RPAREN)) {
            errorAt(DiagCode::E1011, "expected ')' after attribute arguments");
        } else {
            ts_.advance();
        }
        
        // Transfer arguments to arena
        auto builder = arena_.makeBuilder<AttributeArgPtr>();
        for (auto& a : args) {
            builder.push_back(std::move(a));
        }
        attr->args = builder.build();
    }
    
    return attr;
}

// ============================================================================
// 3. ATTRIBUTE ARGUMENT PARSER
// ============================================================================

/**
 * @brief Parses a single literal argument inside an attribute argument list.
 * 
 * Grammar:
 *   attr_arg := STRING_LITERAL | INT_LITERAL | HEX_LITERAL | BINARY_LITERAL
 *             | 'true' | 'false' | IDENTIFIER
 * 
 * @return AttributeArgPtr – parsed node, or nullptr on error.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the literal token
 * On exit:  positioned after the literal token (always advanced)
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Unrecognised token: report error (E1002), advance token, return nullptr
 */
AttributeArgPtr Parser::parseAttributeArgLiteral() {
    SourceLocation loc = ts_.currentLoc();
    
    // String literal
    if (ts_.check(TokenType::STRING_LITERAL)) {
        InternedString value = pool_.intern(ts_.advance().value);
        return arena_.make<AttributeArgAST>(AttributeArgKind::StringLit, value);
    }
    
    // Integer literals (decimal, hex, binary)
    if (ts_.check(TokenType::INT_LITERAL) || 
        ts_.check(TokenType::HEX_LITERAL) ||
        ts_.check(TokenType::BINARY_LITERAL)) {
        InternedString value = pool_.intern(ts_.advance().value);
        return arena_.make<AttributeArgAST>(AttributeArgKind::IntLit, value);
    }
    
    // Boolean literals
    if (ts_.check(TokenType::TRUE) || ts_.check(TokenType::FALSE)) {
        InternedString value = pool_.intern(ts_.advance().value);
        return arena_.make<AttributeArgAST>(AttributeArgKind::BoolLit, value);
    }
    
    // Type identifier (e.g., calling convention name)
    if (ts_.check(TokenType::IDENTIFIER)) {
        InternedString value = pool_.intern(ts_.advance().value);
        return arena_.make<AttributeArgAST>(AttributeArgKind::TypeIdent, value);
    }
    
    // Unexpected token
    errorAt(DiagCode::E1002, 
            "expected string, integer, boolean, or identifier in attribute argument");
    
    // Advance to avoid infinite loop on the same token
    if (!ts_.isAtEnd()) {
        ts_.advance();
    }
    
    return nullptr;
}