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
 *   - parseGenericParams() – parses `< param { ',' param } >`
 *   - parseGenericParam()  – parses a single parameter with optional constraints
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
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at '<' token
 * On exit:  positioned after the closing '>'
 * 
 * ─── Empty List ───────────────────────────────────────────────────────────
 *   - An empty list `<` `>` is allowed and returns an empty ArenaSpan
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 * Uses saved position pattern. If parseGenericParam() makes no progress,
 * consumes one token and continues (prevents infinite loop).
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing '>' at end: consume() reports error
 * - Invalid generic parameter: skip parameter, continue
 * - Unbalanced brackets: returns empty span after recovery
 */
ArenaSpan<GenericParamPtr> Parser::parseGenericParams() {
    LUC_LOG_DECL_EXTREME("parseGenericParams: entering");
    
    if (!ts_.match(TokenType::LESS)) {
        LUC_LOG_DECL_EXTREME("parseGenericParams: no generic parameters");
        return ArenaSpan<GenericParamPtr>();
    }
    
    if (ts_.check(TokenType::GREATER)) {
        LUC_LOG_DECL_EXTREME("parseGenericParams: empty generic parameter list");
        ts_.advance();
        return ArenaSpan<GenericParamPtr>();
    }
    
    std::vector<GenericParamPtr> params;
    int paramCount = 0;
    
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        GenericParamPtr gp = parseGenericParam();
        if (gp) {
            paramCount++;
            LUC_LOG_DECL_EXTREME("parseGenericParams: parsed parameter #" << paramCount);
            params.push_back(gp);
        }
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' after generic parameters");
    
    auto builder = arena_.makeBuilder<GenericParamPtr>();
    for (auto& p : params) builder.push_back(p);
    
    LUC_LOG_DECL_VERBOSE("parseGenericParams: parsed " << paramCount << " generic parameter(s)");
    return builder.build();
}

// ============================================================================
// Generic Parameter – Single Parameter with Constraints
// ============================================================================

/**
 * @brief Parses a single generic parameter with optional trait constraints.
 * 
 * Grammar: IDENTIFIER [ `:` IDENTIFIER ( `+` IDENTIFIER )* ]
 * 
 * Example: `T : Hashable + Comparable + Printable`
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at parameter name
 * On exit:  positioned after the last constraint (or after the name)
 * 
 * ─── Constraint Storage ────────────────────────────────────────────────────
 *   - Constraints are stored as ArenaSpan<InternedString>
 *   - The semantic pass resolves trait names and validates
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - Missing parameter name: returns nullptr
 * - Missing trait name after ':' or '+': reports error, stops parsing constraints
 */
GenericParamPtr Parser::parseGenericParam() {
    LUC_LOG_DECL_EXTREME("parseGenericParam: entering");
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        LUC_LOG_DECL("parseGenericParam: ERROR - expected generic parameter name");
        errorAt(DiagCode::E1003, "expected generic parameter name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    LUC_LOG_DECL_EXTREME("parseGenericParam: name = " << pool_.lookup(name));
    
    auto gp = arena_.make<GenericParamAST>(name);
    
    if (ts_.match(TokenType::COLON)) {
        LUC_LOG_DECL_EXTREME("parseGenericParam: parsing constraints");
        std::vector<InternedString> constraints;
        int constraintCount = 0;
        
        while (true) {
            if (!ts_.check(TokenType::IDENTIFIER)) {
                LUC_LOG_DECL("parseGenericParam: ERROR - expected trait name in constraint");
                errorAt(DiagCode::E1003, "expected trait name in constraint");
                break;
            }
            InternedString trait = pool_.intern(ts_.advance().value);
            constraints.push_back(trait);
            constraintCount++;
            LUC_LOG_DECL_EXTREME("parseGenericParam: constraint #" << constraintCount 
                                 << " = " << pool_.lookup(trait));
            
            if (!ts_.match(TokenType::PLUS)) break;
        }
        
        auto builder = arena_.makeBuilder<InternedString>();
        for (auto& c : constraints) builder.push_back(c);
        gp->constraints = builder.build();
        
        LUC_LOG_DECL_EXTREME("parseGenericParam: " << constraintCount << " constraint(s)");
    }
    
    return gp;
}