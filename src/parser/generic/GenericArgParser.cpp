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
    LUC_LOG_TYPE_EXTREME("parseGenericArgs: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    // Log the current token - the caller should have consumed the '<' already
    LUC_LOG_TYPE("parseGenericArgs: current token (should be after '<') = '" << ts_.peek().value 
                 << "' (type=" << static_cast<int>(ts_.peek().type) 
                 << ") at line " << ts_.peek().line << ", col " << ts_.peek().column);
    
    // The opening '<' is already consumed by the caller.
    if (ts_.check(TokenType::GREATER)) {
        LUC_LOG_TYPE_EXTREME("parseGenericArgs: empty generic args list");
        ts_.advance();
        return ArenaSpan<TypePtr>();
    }
    
    std::vector<TypePtr> args;
    int argCount = 0;
    
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        
        LUC_LOG_TYPE_EXTREME("parseGenericArgs: parsing argument #" << argCount + 1 
                             << " at line " << ts_.peek().line << ", col " << ts_.peek().column);
        
        TypePtr arg = parseGenericArg();
        if (!arg || arg->isa<UnknownTypeAST>()) {
            LUC_LOG_TYPE("parseGenericArgs: ERROR - failed to parse generic argument");
            break;
        }
        argCount++;
        LUC_LOG_TYPE_EXTREME("parseGenericArgs: parsed argument #" << argCount);
        args.push_back(arg);
        
        LUC_LOG_TYPE_EXTREME("parseGenericArgs: after argument, next token = '" << ts_.peek().value 
                             << "' (type=" << static_cast<int>(ts_.peek().type) << ")");
        
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    LUC_LOG_TYPE("parseGenericArgs: expecting '>' at line " << ts_.peek().line 
                 << ", col " << ts_.peek().column);
    ts_.consume(TokenType::GREATER, "expected '>' to close generic arguments");
    
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& a : args) builder.push_back(a);
    
    LUC_LOG_TYPE_VERBOSE("parseGenericArgs: parsed " << argCount << " generic argument(s)");
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
    LUC_LOG_TYPE_EXTREME("parseGenericArg: entering at line " << ts_.currentLoc().line()
                         << ", col " << ts_.currentLoc().column());
    
    size_t savedPos = ts_.getPos();
    TypePtr arg = parseType();
    
    if (ts_.getPos() == savedPos) {
        LUC_LOG_TYPE("parseGenericArg: ERROR - expected type in generic argument list at line "
                     << ts_.peek().line << ", col " << ts_.peek().column);
        errorAt(DiagCode::E1005, "expected type in generic argument list");
        return arena_.make<UnknownTypeAST>();
    }
    
    // Disallow reference types (&T) as generic arguments
    if (arg->isa<RefTypeAST>()) {
        LUC_LOG_TYPE("parseGenericArg: ERROR - reference type cannot be generic argument");
        errorAt(DiagCode::E1026, "Reference type '&T' cannot be used as a generic argument. Use a type alias if needed.");
        return arena_.make<UnknownTypeAST>();
    }
    
    // Disallow pointer types (*T) as generic arguments
    if (arg->isa<PtrTypeAST>()) {
        LUC_LOG_TYPE("parseGenericArg: ERROR - pointer type cannot be generic argument");
        errorAt(DiagCode::E1027, "Pointer type '*T' cannot be used as a generic argument. Use a type alias if needed.");
        return arena_.make<UnknownTypeAST>();
    }
    
    LUC_LOG_TYPE_EXTREME("parseGenericArg: success, parsed type");
    return arg;
}