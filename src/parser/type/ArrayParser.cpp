/**
 * @file ArrayParser.cpp
 * @brief Parses concrete and generic array types.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements array type parsing for the Luc language.
 * 
 * ## Array Kinds
 * 
 * | Kind     | Concrete Syntax | Generic Syntax (with type variable) |
 * |----------|-----------------|--------------------------------------|
 * | Slice    | `[_, T]`        | `[_, <T>]`                           |
 * | Dynamic  | `[*, T]`        | `[*, <T>]`                           |
 * | Fixed    | `[N, T]`        | `[N, <T>]`                           |
 * 
 * ## Two Entry Points
 * 
 *   1. parseArrayType()    – concrete arrays only (no type variables)
 *                            Called from parseBaseType()
 * 
 *   2. parseGenericArray() – generic arrays with type variables
 *                            Called from parseImplDecl(), parseFromDecl(),
 *                            and parseTypeAliasDecl()
 * 
 * @see ArrayTypeAST for concrete array representation
 * @see GenericArrayTypeAST for generic array representation
 */

#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// 1. CONCRETE ARRAY TYPE
// ============================================================================
//
// parseArrayType() parses concrete array types (no type variables).
// It is called from parseBaseType() for normal type annotations.
//
// Grammar:
//   array_type := '[' '_' ',' type ']'      -- slice:   [_, T]
//               | '[' '*' ',' type ']'      -- dynamic: [*, T]
//               | '[' INT_LITERAL ',' type ']' -- fixed:   [N, T]
//
// This function does NOT parse generic arrays like `[_, <T>]`.
//
// Examples:
//   [_, int]   → ArrayTypeAST { kind=Slice,   element=Int }
//   [*, float] → ArrayTypeAST { kind=Dynamic, element=Float }
//   [4, Vec2]  → ArrayTypeAST { kind=Fixed,   size=4, element=Vec2 }
//
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '['
// On exit:  positioned after the element type
//
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing closing ']': reports error
//   - Invalid size literal: reports error, size set to 0
//   - Missing element type: reports error, returns UnknownTypeAST
//   - Unrecognised content: skips to matching ']'
//
// @return TypePtr – ArrayTypeAST on success, UnknownTypeAST on error
// ============================================================================

TypePtr Parser::parseArrayType() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    ArrayKind arrayKind;
    uint64_t fixedSize = 0;

    // Dynamic array: [*, T]
    if (ts_.check(TokenType::MUL)) {
        arrayKind = ArrayKind::Dynamic;
        ts_.advance();
        ts_.consume(TokenType::RBRACKET, "expected ']' after '*'");
        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[*]'");
            return arena_.make<UnknownTypeAST>();
        }
        auto node = arena_.make<ArrayTypeAST>(arrayKind, 0, std::move(elem));
        node->loc = loc;
        return node;
    }

    // Slice: [_, T]
    if (ts_.check(TokenType::WILDCARD)) {
        arrayKind = ArrayKind::Slice;
        ts_.advance();
        ts_.consume(TokenType::RBRACKET, "expected ']' after '_'");
        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[_, '");
            return arena_.make<UnknownTypeAST>();
        }
        auto node = arena_.make<ArrayTypeAST>(arrayKind, 0, std::move(elem));
        node->loc = loc;
        return node;
    }

    // Fixed array: [N, T]
    if (ts_.check(TokenType::INT_LITERAL)) {
        arrayKind = ArrayKind::Fixed;
        Token sizeTok = ts_.advance();
        std::string raw = sizeTok.value;
        raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());
        
        char* end = nullptr;
        fixedSize = std::strtoull(raw.c_str(), &end, 10);
        if (*end != '\0') {
            error(ts_.locOf(sizeTok), DiagCode::E2009, "invalid array size");
            fixedSize = 0;
        }
        
        ts_.consume(TokenType::RBRACKET, "expected ']' after array size");
        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[" + sizeTok.value + ", '");
            return arena_.make<UnknownTypeAST>();
        }
        auto node = arena_.make<ArrayTypeAST>(arrayKind, fixedSize, std::move(elem));
        node->loc = loc;
        return node;
    }

    errorAt(DiagCode::E2001, "expected '_', '*', or integer in array type");
    // Recovery: skip to matching ']'
    int depth = 1;
    while (!ts_.isAtEnd() && depth > 0) {
        if (ts_.check(TokenType::LBRACKET)) depth++;
        else if (ts_.check(TokenType::RBRACKET)) depth--;
        ts_.advance();
    }
    return arena_.make<UnknownTypeAST>();
}

// ============================================================================
// 2. GENERIC ARRAY TYPE (with free type variable)
// ============================================================================
//
// parseGenericArray() parses an array type with a free type variable,
// e.g., `[_, <T>]`, `[*, <T>]`, or `[N, <T>]`.
//
// This form declares a type variable (e.g., `T`) that can be used in
// method signatures or conversion entries. It is ONLY valid in:
//   - `impl` targets:   `impl [_, <T>] { ... }`
//   - `from` targets:   `from [_, <T>] { ... }`
//   - Type alias RHS:   `type List<T> = [_, T]`
//
// Grammar:
//   generic_array := '[' ( '_' | '*' | INT_LITERAL ) ',' '<' IDENTIFIER '>' ']'
//
// Examples:
//   [_, <T>]   → GenericArrayTypeAST { kind=Slice,   typeParamName="T" }
//   [*, <T>]   → GenericArrayTypeAST { kind=Dynamic, typeParamName="T" }
//   [4, <K>]   → GenericArrayTypeAST { kind=Fixed,   size=4, typeParamName="K" }
//
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '['
// On exit:  positioned after the closing ']'
//
// ─── Important ────────────────────────────────────────────────────────────
//   - The type variable name is stored in GenericArrayTypeAST::typeParamName
//   - The semantic pass will introduce this variable into the scope of the
//     impl, from, or type alias block
//   - The variable can then be used as a plain identifier in method signatures
//
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Invalid array kind: reports error, skips to matching ']'
//   - Missing '<' before type variable: reports error
//   - Missing type variable name: reports error
//   - Missing '>' after type variable: consume() reports error
//   - Missing closing ']': consume() reports error
//
// @return TypePtr – GenericArrayTypeAST on success, UnknownTypeAST on error
// ============================================================================

TypePtr Parser::parseGenericArray() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    // Determine array kind
    ArrayKind arrayKind;
    uint64_t fixedSize = 0;

    if (ts_.check(TokenType::WILDCARD)) {
        arrayKind = ArrayKind::Slice;
        ts_.advance();
    } else if (ts_.check(TokenType::MUL)) {
        arrayKind = ArrayKind::Dynamic;
        ts_.advance();
    } else if (ts_.check(TokenType::INT_LITERAL)) {
        arrayKind = ArrayKind::Fixed;
        Token sizeTok = ts_.advance();
        std::string raw = sizeTok.value;
        raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());
        char* end = nullptr;
        fixedSize = std::strtoull(raw.c_str(), &end, 10);
        if (*end != '\0') {
            error(ts_.locOf(sizeTok), DiagCode::E2009, "invalid array size");
            fixedSize = 0;
        }
    } else {
        errorAt(DiagCode::E2001, "expected '_', '*', or integer literal in generic array");
        // Recovery: skip to matching ']'
        int depth = 1;
        while (!ts_.isAtEnd() && depth > 0) {
            if (ts_.check(TokenType::LBRACKET)) depth++;
            else if (ts_.check(TokenType::RBRACKET)) depth--;
            ts_.advance();
        }
        return arena_.make<UnknownTypeAST>();
    }

    ts_.consume(TokenType::COMMA, "expected ',' after array kind");

    // Parse the type variable: '<' IDENTIFIER '>'
    ts_.consume(TokenType::LESS, "expected '<' before type variable in generic array");

    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type variable name after '<'");
        // Still try to recover
        ts_.consume(TokenType::GREATER, "expected '>' after type variable");
        ts_.consume(TokenType::RBRACKET, "expected ']' to close generic array");
        return arena_.make<UnknownTypeAST>();
    }

    InternedString typeParamName = pool_.intern(ts_.advance().value);
    ts_.consume(TokenType::GREATER, "expected '>' after type variable");
    ts_.consume(TokenType::RBRACKET, "expected ']' to close generic array");

    auto node = arena_.make<GenericArrayTypeAST>(arrayKind, fixedSize, typeParamName);
    node->loc = loc;
    return node;
}