/**
 * @file GenericArgParser.cpp
 * @brief Parses generic arguments for type instantiation (call site).
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the parser for generic argument lists used at instantiation
 * sites (e.g., `Box<int>`, `identity<int>(42)`, `impl [_, <T>]`).
 * 
 * ## Functions
 * 
 *   - parseGenericArgs()  – parses a full list: `< type { ',' type } >`
 *   - parseGenericArg()   – parses a single type argument (helper)
 * 
 * @see GenericParamParser.cpp for declaration‑side generic parameters
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Generic Arguments – Full List
// ============================================================================

/**
 * @brief Parses a generic argument list for type instantiation.
 * 
 * Grammar: '<' type { ',' type } '>'
 * 
 * Examples:
 *   <int>
 *   <string, Vec2>
 *   <T, U, V>
 *   <>                    (empty – allowed)
 * 
 * ─── Preconditions ────────────────────────────────────────────────────────
 *   - The caller (parseNamedType, parsePostfixExpr, etc.) has already
 *     consumed the opening '<' token.
 *   - This function starts immediately after the '<'.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 *   - If the next token is '>', consumes it and returns empty span.
 *   - Otherwise parses comma‑separated types using parseGenericArg() until '>'.
 *   - Consumes the closing '>'.
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing '>' at end: consume() reports error.
 *   - Missing type after comma: reports error, breaks loop.
 *   - Empty list '<' '>' is valid (returns empty span).
 * 
 * @return ArenaSpan<TypePtr> – span of type arguments (may be empty).
 */
ArenaSpan<TypePtr> Parser::parseGenericArgs() {
    // The opening '<' is already consumed by the caller.
    // We just need to ensure the token stream is ready.
    // (The caller used ts_.match(TokenType::LESS) or ts_.consume.)
    // This function expects to be called right after the '<'.

    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<TypePtr>();
    }
    
    std::vector<TypePtr> args;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        
        TypePtr arg = parseGenericArg();
        if (!arg || arg->isa<UnknownTypeAST>()) {
            // Error already reported, break to avoid infinite loop
            break;
        }
        args.push_back(std::move(arg));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' to close generic arguments");
    
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}

// ============================================================================
// Generic Argument – Single Type
// ============================================================================

/**
 * @brief Parses a single generic argument (a type).
 * 
 * Grammar: type
 * 
 * Used inside `parseGenericArgs()` to parse each argument.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the first token of a type.
 * On exit:  positioned after the type.
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - If no progress is made (parseType fails to consume any token),
 *     reports error and returns UnknownTypeAST.
 * 
 * @return TypePtr – the parsed type, or UnknownTypeAST on error.
 */
TypePtr Parser::parseGenericArg() {
    size_t savedPos = ts_.getPos();
    TypePtr arg = parseType();
    if (ts_.getPos() == savedPos) {
        errorAt(DiagCode::E2005, "expected type in generic argument list");
        return arena_.make<UnknownTypeAST>();
    }
    return arg;
}