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
    if (!ts_.match(TokenType::LESS)) return ArenaSpan<GenericParamPtr>();
    
    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<GenericParamPtr>();
    }
    
    std::vector<GenericParamPtr> params;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        GenericParamPtr gp = parseGenericParam();
        if (gp) params.push_back(std::move(gp));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' after generic parameters");
    
    auto builder = arena_.makeBuilder<GenericParamPtr>();
    for (auto& p : params) builder.push_back(std::move(p));
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
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E1003, "expected generic parameter name");
        return nullptr;
    }
    InternedString name = pool_.intern(ts_.advance().value);
    auto gp = arena_.make<GenericParamAST>(name);
    
    if (ts_.match(TokenType::COLON)) {
        std::vector<InternedString> constraints;
        while (true) {
            if (!ts_.check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E1003, "expected trait name in constraint");
                break;
            }
            constraints.push_back(pool_.intern(ts_.advance().value));
            if (!ts_.match(TokenType::PLUS)) break;
        }
        auto builder = arena_.makeBuilder<InternedString>();
        for (auto& c : constraints) builder.push_back(std::move(c));
        gp->constraints = builder.build();
    }
    
    return gp;
}