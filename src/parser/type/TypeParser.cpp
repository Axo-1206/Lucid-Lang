#include "parser/Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

// ============================================================================
// Root Type Parsers
// ============================================================================
// 
// parseType() and parseTypeWithNullable() are the entry points for parsing
// type annotations.
// 
//   parseType()               → alias for parseTypeWithNullable()
//   parseTypeWithNullable()   → parses base_type followed by optional '?'
// 
// The '?' suffix is only valid on value types (primitives, structs, arrays,
// named aliases). The semantic pass enforces this restriction.
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at the first token of a type
// On exit:  positioned after the type (and optional '?' if present)
// 
// ─── Return Value ─────────────────────────────────────────────────────────
// Returns TypePtr (never nullptr; on error returns UnknownTypeAST)
// ============================================================================

TypePtr Parser::parseType() {
    return parseTypeWithNullable();
}

/**
 * @brief Parses a type annotation with optional `?` and `!` suffixes.
 * 
 * Grammar:
 *   type_with_suffix := base_type [ '?' ] [ result_suffix ]
 *   result_suffix    := '!' type
 *                    | '!'
 * 
 * The `?` suffix attaches to the base type (making it nullable).
 * The `!` suffix attaches after `?` (if present) and creates a result type.
 * 
 * Examples:
 *   int           → PrimitiveTypeAST(Int)
 *   int?          → NullableTypeAST(PrimitiveTypeAST(Int))
 *   int!string    → ResultTypeAST(PrimitiveTypeAST(Int), PrimitiveTypeAST(String))
 *   int?!string   → ResultTypeAST(NullableTypeAST(Int), PrimitiveTypeAST(String))
 *   int!          → ResultTypeAST(PrimitiveTypeAST(Int), nullptr)  -- bare '!'
 * 
 * ─── Important Grammar Rules (enforced by semantic pass) ───────────────────
 *   - `?` always comes before `!` when both are present
 *   - Neither `inner` nor `errorType` in ResultTypeAST may themselves carry `!`
 *   - `!` is NOT valid directly on an array type or inline function type
 *   - Bare `!` (no error type) means failure carries nil
 * 
 * ─── Token Consumption ─────────────────────────────────────────────────────
 * On entry: positioned at the first token of a base type
 * On exit:  positioned after all suffixes ('?' and/or '!')
 * 
 * ─── Error Recovery ───────────────────────────────────────────────────────
 * - If '!' is present but the error type cannot be parsed, errorType remains
 *   nullptr (treated as bare '!' for recovery)
 * - Malformed error type is reported via errorAt()
 * 
 * @return TypePtr – never nullptr; returns UnknownTypeAST on unrecoverable error
 */
TypePtr Parser::parseTypeWithNullable() {
    TypePtr ty = parseBaseType();
    if (ty && ts_.match(TokenType::QUESTION)) {
        ty = arena_.make<NullableTypeAST>(std::move(ty));
    }
    // Result suffix (T!E)
    if (ty && ts_.match(TokenType::BANG)) {
        TypePtr errorType = nullptr;
        if (looksLikeType()) {
            errorType = parseType();
        }
        // errorType may be nullptr for bare '!'
        ty = arena_.make<ResultTypeAST>(std::move(ty), std::move(errorType));
    }
    return ty;
}


// ============================================================================
// Base Type Dispatcher
// ============================================================================
// 
// parseBaseType() dispatches to the appropriate concrete type parser based on
// the current token. It does NOT consume the optional '?' suffix.
// 
// Dispatch Priority (highest to lowest):
//   1. Primitive keywords (int, float, string, etc.)
//   2. IDENTIFIER (user-defined types)
//   3. '[' (array types)
//   4. '&' (reference types)
//   5. '*' (pointer types)
//   6. '(' or '~' (function types)
//   7. default → error + UnknownTypeAST
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at the first token of a base type
// On exit:  positioned after the base type (before any '?' suffix)
// ============================================================================
TypePtr Parser::parseBaseType() {
    switch (ts_.peekType()) {
        case TokenType::TYPE_BOOL:
        case TokenType::TYPE_BYTE:
        case TokenType::TYPE_SHORT:
        case TokenType::TYPE_INT:
        case TokenType::TYPE_LONG:
        case TokenType::TYPE_UBYTE:
        case TokenType::TYPE_USHORT:
        case TokenType::TYPE_UINT:
        case TokenType::TYPE_ULONG:
        case TokenType::TYPE_INT8:
        case TokenType::TYPE_INT16:
        case TokenType::TYPE_INT32:
        case TokenType::TYPE_INT64:
        case TokenType::TYPE_UINT8:
        case TokenType::TYPE_UINT16:
        case TokenType::TYPE_UINT32:
        case TokenType::TYPE_UINT64:
        case TokenType::TYPE_FLOAT:
        case TokenType::TYPE_DOUBLE:
        case TokenType::TYPE_DECIMAL:
        case TokenType::TYPE_STRING:
        case TokenType::TYPE_CHAR:
        case TokenType::TYPE_ANY:
            return parsePrimitiveType();

        case TokenType::IDENTIFIER:
            return parseNamedType();

        case TokenType::LBRACKET:
            return parseArrayType();

        case TokenType::AMPERSAND:
            return parseRefType();

        case TokenType::MUL:
            return parsePtrType();

        case TokenType::LPAREN:
        case TokenType::TILDE:
            return parseFuncType();

        default:
            errorAt(DiagCode::E2005, "expected type, got '" + ts_.peek().value + "'");
            return arena_.make<UnknownTypeAST>();
    }
}