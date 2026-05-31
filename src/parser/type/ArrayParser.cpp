#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Array Type
// ============================================================================
// 
// parseArrayType() parses fixed, slice, and dynamic array types.
// 
// Grammar:
//   array_type := '[' '_' ',' type ']'         -- slice:   [_, T]
//               | '[' '*' ',' type ']'         -- dynamic: [*, T]
//               | '[' INT_LITERAL ',' type ']' -- fixed:   [N, T]
// 
// Examples:
//   [_, int]   → ArrayTypeAST { kind=Slice,   element=Int }
//   [*, float] → ArrayTypeAST { kind=Dynamic, element=Float }
//   [4, Vec2]  → ArrayTypeAST { kind=Fixed,   size=4, element=Vec2 }
//   [4, [4, float]] → nested: fixed array of fixed arrays
// 
// Note: This function parses ONLY concrete array types. For generic arrays
//       with a free type variable (e.g., `[_, <T>]`), use parseArrayTarget()
//       which is called from parseImplDecl.
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '['
// On exit:  positioned after the element type
// 
// ─── Multidimensional Arrays ──────────────────────────────────────────────
//   Handled naturally because the element type is parsed via parseType(),
//   which recursively calls parseArrayType() if the element starts with '['.
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
//   - Uses bracket depth tracking when skipping to matching ']' on error
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing closing ']' after '*' or size: reports error
//   - Invalid size literal: reports error, size set to 0
//   - Missing element type: reports error, returns UnknownTypeAST
//   - Unrecognised content inside brackets: skips to matching ']'
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

/**
 * @brief Parses an array target for an `impl` block.
 *
 * This function handles both concrete arrays (e.g., `[_, int]`) and generic
 * arrays with a free type variable (e.g., `[_, <T>]`).
 *
 * Grammar:
 *   array_target := '[' ( '_' | '*' | INT_LITERAL ) ',' ( type | '<' IDENTIFIER '>' ) ']'
 *
 * Note: The generic form `<IDENTIFIER>` is only allowed inside an `impl` target
 * (or equivalently inside a type alias). For normal type annotations, only the
 * concrete type form is allowed.
 *
 * @return TypePtr – one of ArrayTypeAST (concrete) or GenericArrayTypeAST
 */
TypePtr Parser::parseArrayTarget() {
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
        errorAt(DiagCode::E2001, "expected '_', '*', or integer literal in array target");
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

    // Parse element type (concrete or generic variable)
    TypePtr elemType;
    bool isGeneric = false;
    InternedString typeParamName;

    if (ts_.check(TokenType::LESS)) {
        // Generic array target: '<' IDENTIFIER '>'
        ts_.advance(); // consume '<'
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected type variable name after '<'");
        } else {
            typeParamName = pool_.intern(ts_.advance().value);
            isGeneric = true;
        }
        ts_.consume(TokenType::GREATER, "expected '>' after type variable");
    } else {
        // Concrete element type
        elemType = parseType();
        if (!elemType || elemType->isa<UnknownTypeAST>()) {
            errorAt(DiagCode::E2005, "expected element type for array");
            elemType = arena_.make<UnknownTypeAST>();
        }
    }

    ts_.consume(TokenType::RBRACKET, "expected ']' to close array target");

    if (isGeneric) {
        // Create generic array node
        auto node = arena_.make<GenericArrayTypeAST>(arrayKind, fixedSize, typeParamName);
        node->loc = loc;
        return node;
    } else {
        // Create concrete array node
        auto node = arena_.make<ArrayTypeAST>(arrayKind, fixedSize, std::move(elemType));
        node->loc = loc;
        return node;
    }
}