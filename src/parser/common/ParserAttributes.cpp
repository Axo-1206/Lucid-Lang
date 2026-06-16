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
#include "debug/DebugMacros.hpp"

// ============================================================================
// 1. ATTRIBUTE COLLECTION
// ============================================================================

/**
 * @brief Parses a sequence of attributes preceding a declaration.
 * 
 * Grammar: { '@' IDENTIFIER [ '(' attr_arg_list ')' ] }
 * 
 * @return std::vector<AttributePtr> – may be empty.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * This function does NOT consume any tokens directly. It calls parseAttribute()
 * which consumes each attribute and its arguments.
 */
std::vector<AttributePtr> Parser::parseAttributes() {
    std::vector<AttributePtr> attrs;
    int attrCount = 0;
    
    while (ts_.check(TokenType::AT_SIGN)) {
        AttributePtr attr = parseAttribute();
        if (attr) {
            attrCount++;
            LOG_DECL_EXTREME("parseAttributes: parsed attribute #" << attrCount);
            attrs.push_back(attr);
        }
        // On parse failure, parseAttribute already advanced the token stream
        // so we don't loop infinitely.
    }
    
    if (attrCount > 0) {
        LOG_DECL_EXTREME("parseAttributes: total " << attrCount << " attribute(s)");
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
 * - Invalid argument literal: parseAttributeArgLiteral() reports error and
 *   advances; the argument is skipped, parsing continues
 */
AttributePtr Parser::parseAttribute() {
    SourceLocation loc = ts_.currentLoc();
    
    // Check for '@' token
    if (!ts_.check(TokenType::AT_SIGN)) {
        LOG_DECL("parseAttribute: ERROR - expected '@'");
        errorAt(DiagCode::E1007, "@", "before starting attribute name", ts_.peek().value);
        return nullptr;
    }
    ts_.advance(); // Consume '@'
    
    // Attribute name is required – syntax error if missing
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseAttribute: ERROR - expected attribute name after '@'");
        errorAt(DiagCode::E1002, "attribute name after '@'");
        // Advance to avoid infinite loop on the same token
        if (!ts_.isAtEnd()) {
            ts_.advance();
        }
        return nullptr;
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseAttribute: name = @" << pool_.lookup(name));
    
    auto attr = arena_.make<AttributeAST>();
    attr->loc = loc;
    attr->name = name;
    
    // Parse optional argument list
    if (ts_.check(TokenType::LPAREN)) {
        LOG_DECL_EXTREME("parseAttribute: parsing argument list");
        ts_.advance(); // Consume '('
        
        std::vector<AttributeArgPtr> args;
        int argCount = 0;
        
        // Parse arguments until closing ')'
        while (!ts_.check(TokenType::RPAREN) && !ts_.isAtEnd()) {
            // Handle commas between arguments
            if (!args.empty()) {
                if (!ts_.check(TokenType::COMMA)) {
                    LOG_DECL("parseAttribute: ERROR - expected ',' between attribute arguments");
                    errorAt(DiagCode::E1007, ",", "between attribute arguments", ts_.peek().value);
                    // Skip to next argument or closing parenthesis
                    while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                        ts_.advance();
                    }
                    if (ts_.check(TokenType::COMMA)) {
                        ts_.advance(); // Consume comma and continue
                    } else {
                        break;
                    }
                } else {
                    ts_.advance(); // Consume ','
                }
            }
            
            // Check for empty argument (e.g., trailing comma)
            if (ts_.check(TokenType::RPAREN)) {
                LOG_DECL("parseAttribute: WARNING - trailing comma in attribute arguments");
                errorAt(DiagCode::E1107, "in attribute arguments");
                break;
            }
            
            AttributeArgPtr arg = parseAttributeArgLiteral();
            if (arg) {
                argCount++;
                args.push_back(arg);
            } else {
                // Invalid argument – parseAttributeArgLiteral() already advanced
                // Skip to next comma or closing parenthesis
                while (!ts_.isAtEnd() && !ts_.check(TokenType::COMMA) && !ts_.check(TokenType::RPAREN)) {
                    ts_.advance();
                }
            }
        }
        
        LOG_DECL_EXTREME("parseAttribute: parsed " << argCount << " argument(s)");
        
        // Expect closing ')'
        if (!ts_.check(TokenType::RPAREN)) {
            LOG_DECL("parseAttribute: ERROR - expected ')' after attribute arguments");
            errorAt(DiagCode::E1005, ")", "attribute arguments", ts_.peek().value);
            // Skip to find ')' or end of file
            while (!ts_.isAtEnd() && !ts_.check(TokenType::RPAREN)) {
                ts_.advance();
            }
            if (ts_.check(TokenType::RPAREN)) {
                ts_.advance(); // Consume ')'
            }
        } else {
            ts_.advance(); // Consume ')'
        }
        
        // Transfer arguments to arena
        auto builder = arena_.makeBuilder<AttributeArgPtr>();
        for (auto& a : args) {
            builder.push_back(a);
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
        LOG_DECL_EXTREME("parseAttributeArgLiteral: string literal");
        return arena_.make<AttributeArgAST>(AttributeArgKind::StringLit, value);
    }
    
    // Integer literals (decimal, hex, binary)
    if (ts_.check(TokenType::INT_LITERAL) || 
        ts_.check(TokenType::HEX_LITERAL) ||
        ts_.check(TokenType::BINARY_LITERAL)) {
        InternedString value = pool_.intern(ts_.advance().value);
        LOG_DECL_EXTREME("parseAttributeArgLiteral: integer literal");
        return arena_.make<AttributeArgAST>(AttributeArgKind::IntLit, value);
    }
    
    // Boolean literals
    if (ts_.check(TokenType::TRUE) || ts_.check(TokenType::FALSE)) {
        InternedString value = pool_.intern(ts_.advance().value);
        LOG_DECL_EXTREME("parseAttributeArgLiteral: boolean literal");
        return arena_.make<AttributeArgAST>(AttributeArgKind::BoolLit, value);
    }
    
    // Type identifier (e.g., calling convention name)
    if (ts_.check(TokenType::IDENTIFIER)) {
        InternedString value = pool_.intern(ts_.advance().value);
        LOG_DECL_EXTREME("parseAttributeArgLiteral: type identifier");
        return arena_.make<AttributeArgAST>(AttributeArgKind::TypeIdent, value);
    }
    
    // Unexpected token
    LOG_DECL("parseAttributeArgLiteral: ERROR - unexpected token");
    errorAt(DiagCode::E1106, ts_.peek().value);
    
    // Advance to avoid infinite loop on the same token
    if (!ts_.isAtEnd()) {
        ts_.advance();
    }
    
    return nullptr;
}