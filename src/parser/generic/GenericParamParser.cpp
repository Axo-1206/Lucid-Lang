/**
 * @file GenericParamParser.cpp
 * @brief Parses generic type parameters on declarations (definition side).
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the parser for generic parameter lists used in
 * declarations of functions, structs, traits, impl blocks, and type aliases.
 * 
 * ## Functions
 * 
 *   - parseGenericParamDecls() – parses `< param { ',' param } >`
 *   - parseGenericParamDecl()  – parses a single parameter with optional constraints
 * 
 * @see GenericArgParser.cpp for call‑site generic arguments
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"
#include "debug/DebugMacros.hpp"

// ============================================================================
// Generic Parameters – Full List
// ============================================================================

/**
 * @brief Parses generic type parameters on a declaration.
 * 
 * Grammar: `<` generic_param (`,` generic_param)* `>`
 * 
 * Example: `<T, U : Drawable, V>`
 * 
 * ─── Precondition ─────────────────────────────────────────────────────────
 * Caller MUST have already verified that the current token is LESS ('<').
 * This function assumes it is positioned at the '<' token.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '<' token
 * On exit:  positioned after the closing '>'
 * 
 * ─── Empty List ───────────────────────────────────────────────────────────
 *   - An empty list `<` `>` is allowed and returns an empty ArenaSpan
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern. If parseGenericParamDecl() makes no progress,
 * consumes one token and continues (prevents infinite loop).
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '<' at start: returns empty span (caller error)
 * - Missing '>' at end: reports error, returns what was parsed
 * - Invalid generic parameter: skip parameter, continue
 * - Unbalanced brackets: returns empty span after recovery
 * 
 * @return ArenaSpan of GenericParamDeclPtr on success, empty span on error
 */
ArenaSpan<GenericParamDeclPtr> Parser::parseGenericParamDecls() {
    LOG_DECL_EXTREME("parseGenericParamDecls: entering");
    
    // Check for '<' token (should be present if called correctly)
    if (!ts_.check(TokenType::LESS)) {
        LOG_DECL_EXTREME("parseGenericParamDecls: no generic parameters");
        return ArenaSpan<GenericParamDeclPtr>();
    }
    ts_.advance(); // Consume '<'
    
    // Handle empty generic parameter list: `< >`
    if (ts_.check(TokenType::GREATER)) {
        LOG_DECL_EXTREME("parseGenericParamDecls: empty generic parameter list");
        ts_.advance(); // Consume '>'
        return ArenaSpan<GenericParamDeclPtr>();
    }
    
    std::vector<GenericParamDeclPtr> params;
    int paramCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;
    
    do {
        // Skip optional commas between parameters
        ts_.match(TokenType::COMMA);
        
        // Check if we've reached the end
        if (ts_.check(TokenType::GREATER)) break;
        
        size_t savedPos = ts_.getPos();
        GenericParamDeclPtr gp = parseGenericParamDecl();
        
        if (gp) {
            paramCount++;
            LOG_DECL_EXTREME("parseGenericParamDecls: parsed parameter #" << paramCount);
            params.push_back(gp);
            consecutiveFailures = 0;
        } else {
            consecutiveFailures++;
            LOG_DECL("parseGenericParamDecls: ERROR - failed to parse generic parameter (attempt " 
                     << consecutiveFailures << ")");
            
            // Check for progress
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                // No progress - force consume a token
                LOG_DECL("parseGenericParamDecls: no progress, forcing token consumption");
                ts_.advance();
            }
            
            // Skip to next potential parameter start
            while (!ts_.isAtEnd() && 
                   !ts_.check(TokenType::GREATER) && 
                   !ts_.check(TokenType::IDENTIFIER)) {
                ts_.advance();
            }
        }
        
        // Safety: prevent infinite loops
        if (consecutiveFailures > 5) {
            LOG_DECL("parseGenericParamDecls: too many consecutive failures, forcing skip to '>'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::GREATER)) {
                ts_.advance();
            }
            break;
        }
        
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES);
    
    // Expect closing '>'
    if (!ts_.check(TokenType::GREATER)) {
        LOG_DECL("parseGenericParamDecls: ERROR - expected '>' after generic parameters");
        errorAt(DiagCode::E1005, ">", "generic parameter list");
        // Return what we have so far
    } else {
        ts_.advance(); // Consume '>'
    }
    
    // Build the ArenaSpan
    auto builder = arena_.makeBuilder<GenericParamDeclPtr>();
    for (auto& p : params) builder.push_back(p);
    
    LOG_DECL_VERBOSE("parseGenericParamDecls: parsed " << paramCount << " generic parameter(s)");
    return builder.build();
}

// ============================================================================
// Generic Parameter – Single Parameter with Constraints
// ============================================================================

/**
 * @brief Parses a single generic parameter with optional trait constraints.
 * 
 * Grammar: IDENTIFIER [ `:` type ( `+` type )* ]
 * 
 * Example: `T : Hashable + Comparable + Printable`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at parameter name
 * On exit:  positioned after the last constraint (or after the name)
 * 
 * ─── Constraint Storage ────────────────────────────────────────────────────
 *   - Constraints are stored as ArenaSpan<TypeAST*> (full type nodes)
 *   - This supports generic trait constraints like `Iterator<Item=U>`
 *   - The semantic pass resolves trait types and validates
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing parameter name: returns nullptr
 * - Missing trait type after ':' or '+': reports error, stops parsing constraints
 * 
 * @return GenericParamDeclPtr on success, nullptr on error
 */
GenericParamDeclPtr Parser::parseGenericParamDecl() {
    LOG_DECL_EXTREME("parseGenericParamDecl: entering");
    
    // Check for parameter name
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LOG_DECL("parseGenericParamDecl: ERROR - expected generic parameter name");
        errorAt(DiagCode::E1002, "generic parameter name",ts_.peek().value);
        return nullptr;
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    LOG_DECL_EXTREME("parseGenericParamDecl: name = " << pool_.lookup(name));
    
    // Create the generic parameter node
    auto* gp = arena_.make<GenericParamDeclAST>(name);
    
    // Parse optional constraints
    if (ts_.match(TokenType::COLON)) {
        LOG_DECL_EXTREME("parseGenericParamDecl: parsing constraints");
        std::vector<TypeAST*> constraints;
        int constraintCount = 0;
        
        while (true) {
            // Parse constraint type (can be a simple trait name or generic trait like `Iterator<Item=U>`)
            TypePtr constraintType = parseType();
            if (!constraintType) {
                LOG_DECL("parseGenericParamDecl: ERROR - expected type in constraint");
                errorAt(DiagCode::E1002, "trait type in generic contraint", ts_.peek().value);
                break;
            }
            
            constraints.push_back(constraintType);
            constraintCount++;
            LOG_DECL_EXTREME("parseGenericParamDecl: constraint #" << constraintCount);
            
            // Check for additional constraints with '+'
            if (!ts_.match(TokenType::PLUS)) break;
        }
        
        // Build the constraints span
        auto builder = arena_.makeBuilder<TypeAST*>();
        for (auto& c : constraints) builder.push_back(c);
        gp->constraints = builder.build();
        
        LOG_DECL_EXTREME("parseGenericParamDecl: " << constraintCount << " constraint(s)");
    }
    
    LOG_DECL_EXTREME("parseGenericParamDecl: success - parameter '" << pool_.lookup(name) << "'");
    return gp;
}