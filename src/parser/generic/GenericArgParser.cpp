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
#include "debug/DebugMacros.hpp"

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
 *     verified and consumed the opening '<' token.
 *   - This function starts immediately after the '<'.
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 *   - If the next token is '>', consumes it and returns empty span.
 *   - Otherwise parses comma‑separated types using parseGenericArg() until '>'.
 *   - Consumes the closing '>'.
 * 
 * ─── Loop Safety ──────────────────────────────────────────────────────────
 *   - Uses saved position pattern with consecutive failure counter.
 *   - If parseGenericArg() makes no progress, consumes token and continues.
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - Missing '>' at end: reports error (E1005), returns what was parsed.
 *   - Missing type after comma: reports error, breaks loop.
 *   - Empty list '<' '>' is valid (returns empty span).
 * 
 * @return ArenaSpan<TypePtr> – span of type arguments (may be empty).
 */
ArenaSpan<TypePtr> Parser::parseGenericArgs() {
    LOG_TYPE_EXTREME("parseGenericArgs: entering at line " << ts_.currentLoc().line()
                     << ", col " << ts_.currentLoc().column());
    
    // Note: The opening '<' is already consumed by the caller.
    // Check for empty generic args list: `< >`
    if (ts_.check(TokenType::GREATER)) {
        LOG_TYPE_EXTREME("parseGenericArgs: empty generic args list");
        ts_.advance(); // Consume '>'
        return ArenaSpan<TypePtr>();
    }
    
    std::vector<TypePtr> args;
    int argCount = 0;
    int consecutiveFailures = 0;
    const int MAX_CONSECUTIVE_FAILURES = 10;
    
    do {
        // Skip optional commas between arguments
        ts_.match(TokenType::COMMA);
        
        // Check if we've reached the end
        if (ts_.check(TokenType::GREATER)) break;
        
        // Check for trailing comma error
        if (ts_.check(TokenType::COMMA)) {
            LOG_TYPE("parseGenericArgs: ERROR - unexpected trailing comma");
            errorAt(DiagCode::E1107, "generic argument list");
            break;
        }
        
        size_t savedPos = ts_.getPos();
        LOG_TYPE_EXTREME("parseGenericArgs: parsing argument #" << argCount + 1 
                         << " at line " << ts_.peek().line << ", col " << ts_.peek().column);
        
        TypePtr arg = parseGenericArg();
        
        if (arg && !arg->isa<UnknownTypeAST>()) {
            argCount++;
            LOG_TYPE_EXTREME("parseGenericArgs: parsed argument #" << argCount);
            args.push_back(arg);
            consecutiveFailures = 0;
        } else {
            consecutiveFailures++;
            LOG_TYPE("parseGenericArgs: ERROR - failed to parse generic argument (attempt " 
                     << consecutiveFailures << ")");
            
            // Check for progress
            if (ts_.getPos() == savedPos && !ts_.isAtEnd()) {
                LOG_TYPE("parseGenericArgs: no progress, forcing token consumption");
                ts_.advance();
            }
            
            // Skip to next potential argument start
            while (!ts_.isAtEnd() && 
                   !ts_.check(TokenType::GREATER) && 
                   !ts_.check(TokenType::COMMA) &&
                   !ts_.check(TokenType::IDENTIFIER) &&
                   !ts_.isPrimitiveTypeToken(ts_.peekType())) {
                ts_.advance();
            }
        }
        
        // Safety: prevent infinite loops
        if (consecutiveFailures > 5) {
            LOG_TYPE("parseGenericArgs: too many consecutive failures, forcing skip to '>'");
            while (!ts_.isAtEnd() && !ts_.check(TokenType::GREATER)) {
                ts_.advance();
            }
            break;
        }
        
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd() && consecutiveFailures < MAX_CONSECUTIVE_FAILURES);
    
    // Expect closing '>'
    if (!ts_.check(TokenType::GREATER)) {
        LOG_TYPE("parseGenericArgs: ERROR - expected '>' to close generic arguments");
        errorAt(DiagCode::E1005, ">", "generic argument list", ts_.peek().value);
    } else {
        ts_.advance(); // Consume '>'
    }
    
    // Build the ArenaSpan
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& a : args) builder.push_back(a);
    
    LOG_TYPE_VERBOSE("parseGenericArgs: parsed " << argCount << " generic argument(s)");
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
 * ─── Invalid Generic Argument Types ───────────────────────────────────────
 *   - Reference types (&T) are not allowed as generic arguments
 *   - Pointer types (*T) are not allowed as generic arguments
 *   - These restrictions are enforced by the language specification.
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 *   - If no progress is made (parseType fails to consume any token),
 *     reports error (E1109) and returns UnknownTypeAST.
 *   - If reference type is used, reports error (E1110) and returns UnknownTypeAST.
 *   - If pointer type is used, reports error (E1111) and returns UnknownTypeAST.
 * 
 * @return TypePtr – the parsed type, or UnknownTypeAST on error.
 */
TypePtr Parser::parseGenericArg() {
    LOG_TYPE_EXTREME("parseGenericArg: entering at line " << ts_.currentLoc().line()
                     << ", col " << ts_.currentLoc().column());
    
    size_t savedPos = ts_.getPos();
    TypePtr arg = parseType();
    
    // Check if any progress was made
    if (ts_.getPos() == savedPos) {
        LOG_TYPE("parseGenericArg: ERROR - expected type in generic argument list");
        errorAt(DiagCode::E1008, "in generic argument list", ts_.peek().value);
        return arena_.make<UnknownTypeAST>();
    }
    
    // Disallow reference types (&T) as generic arguments
    if (arg->isa<RefTypeAST>()) {
        LOG_TYPE("parseGenericArg: ERROR - reference type cannot be generic argument");
        errorAt(DiagCode::E1109, ts_.peek().value + " (reference)");
        return arena_.make<UnknownTypeAST>();
    }
    
    // Disallow pointer types (*T) as generic arguments
    if (arg->isa<PtrTypeAST>()) {
        LOG_TYPE("parseGenericArg: ERROR - pointer type cannot be generic argument");
        errorAt(DiagCode::E1109, ts_.peek().value + " (pointer)");
        return arena_.make<UnknownTypeAST>();
    }
    
    LOG_TYPE_EXTREME("parseGenericArg: success, parsed type");
    return arg;
}