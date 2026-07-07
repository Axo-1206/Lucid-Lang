/**
 * @file ParserType.cpp
 * @brief Implementation of type parsers.
 * 
 * This file implements all type parsing functions:
 * - Primitive types (int, float, bool, string, etc.)
 * - Named types (user-defined types)
 * - Array types ([*]T, [_]T, [N]T)
 * - Reference types (&T)
 * - Pointer types (ptr<T>)
 * - Function types ((params) -> return)
 * - Type with nullable/fallible modifiers (T?, T!, T?!)
 * 
 * The type parser uses a Pratt-style recursive descent approach:
 * - parseType() is the main entry point
 * - parseBaseType() handles the core type syntax
 * - parseTypeWithQualifier() handles postfix modifiers
 */

#include "Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

namespace parser {

// =============================================================================
// parseType() - Main entry point for type parsing
// =============================================================================

/**
 * @brief Parse a type with optional nullable/fallible modifiers.
 * 
 * This is the main entry point for type parsing. It parses the base type
 * and then handles any trailing ? or ! modifiers.
 * 
 * Grammar: type = base_type [ '?' ] [ '!' ]  (where '?!' is the only valid order)
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return TypeAST* The parsed type, or nullptr on error
 */
TypeAST* parseType(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseType: parsing type");
    
    // ─── 1. Parse the base type ──────────────────────────────────────────
    TypePtr type = parseBaseType(stream, ctx);
    if (!type) {
        return nullptr;
    }
    
    // ─── 2. Handle nullable/fallible modifiers ──────────────────────────
    return parseTypeWithQualifier(stream, ctx, type);
}

// =============================================================================
// parseBaseType() - Parse the core type without modifiers
// =============================================================================

/**
 * @brief Parse a base type without nullable/fallible modifiers.
 * 
 * This function parses the core type syntax and dispatches to specific
 * type parsers based on the current token.
 * 
 * Grammar: 
 *   base_type = primitive_type
 *             | named_type
 *             | array_type
 *             | ref_type
 *             | ptr_type
 *             | func_type
 *             | '(' type ')'
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return TypeAST* The parsed base type, or nullptr on error
 */
TypeAST* parseBaseType(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseBaseType: parsing base type");
    
    // Check for primitive types using the new helper
    if (stream.isPrimitiveTypeToken(stream.peekType())) {
        return parsePrimitiveType(stream, ctx);
    }
    
    // Check for array type: [*]T, [_]T, [N]T
    if (stream.check(TokenType::LBRACKET)) {
        return parseArrayType(stream, ctx);
    }
    
    // Check for reference type: &T
    if (stream.check(TokenType::AMPERSAND)) {
        return parseRefType(stream, ctx);
    }
    
    // Check for pointer type: ptr<T>
    if (stream.check(TokenType::MUL)) {
        return parsePtrType(stream, ctx);
    }
    
    // Check for function type: (params) -> return
    if (stream.check(TokenType::LPAREN)) {
        return parseFuncType(stream, ctx);
    }
    
    // // Check for parenthesized type: (type)
    // if (stream.check(TokenType::LPAREN)) {
    //     SourceLocation loc = stream.currentLoc();
    //     stream.advance(); // Consume '('
        
    //     TypePtr inner = parseType(stream, ctx);
    //     if (!inner) {
    //         ctx.error(stream, DiagCode::E1003, "type", stream.peekValue());
    //         synchronize(stream, ctx);
    //         return nullptr;
    //     }
        
    //     if (!stream.check(TokenType::RPAREN)) {
    //         ctx.error(stream, DiagCode::E1005, ")", "parenthesized type", stream.peekValue());
    //         synchronizeTo(stream, ctx, TokenType::RPAREN);
    //         return inner;
    //     }
    //     stream.advance(); // Consume ')'
        
    //     return inner;
    // }
    
    // Parse as named type (identifier)
    if (stream.check(TokenType::IDENTIFIER)) {
        return parseNamedType(stream, ctx);
    }
    
    // Unknown type
    ctx.error(stream, DiagCode::E1003, "type", stream.peekValue());
    synchronize(stream, ctx);
    return nullptr;
}

// =============================================================================
// parsePrimitiveType() - Parse primitive types
// =============================================================================

/**
 * @brief Parse a primitive type.
 * 
 * Grammar: primitive_type = "bool" | "int" | "float" | "string" | "char" | ...
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return PrimitiveTypeAST* The parsed primitive type, or nullptr on error
 */
TypeAST* parsePrimitiveType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Use the new helper to check for primitive type
    if (!stream.isPrimitiveTypeToken(stream.peekType())) {
        ctx.error(stream, DiagCode::E1003, "primitive type", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    Token tok = stream.advance();
    PrimitiveKind kind;
    
    switch (tok.type) {
        case TokenType::TYPE_BOOL:   kind = PrimitiveKind::Bool; break;
        case TokenType::TYPE_BYTE:   kind = PrimitiveKind::Byte; break;
        case TokenType::TYPE_SHORT:  kind = PrimitiveKind::Short; break;
        case TokenType::TYPE_INT:    kind = PrimitiveKind::Int; break;
        case TokenType::TYPE_LONG:   kind = PrimitiveKind::Long; break;
        case TokenType::TYPE_UBYTE:  kind = PrimitiveKind::Ubyte; break;
        case TokenType::TYPE_USHORT: kind = PrimitiveKind::Ushort; break;
        case TokenType::TYPE_UINT:   kind = PrimitiveKind::Uint; break;
        case TokenType::TYPE_ULONG:  kind = PrimitiveKind::Ulong; break;
        case TokenType::TYPE_INT8:   kind = PrimitiveKind::Int8; break;
        case TokenType::TYPE_INT16:  kind = PrimitiveKind::Int16; break;
        case TokenType::TYPE_INT32:  kind = PrimitiveKind::Int32; break;
        case TokenType::TYPE_INT64:  kind = PrimitiveKind::Int64; break;
        case TokenType::TYPE_UINT8:  kind = PrimitiveKind::Uint8; break;
        case TokenType::TYPE_UINT16: kind = PrimitiveKind::Uint16; break;
        case TokenType::TYPE_UINT32: kind = PrimitiveKind::Uint32; break;
        case TokenType::TYPE_UINT64: kind = PrimitiveKind::Uint64; break;
        case TokenType::TYPE_FLOAT:  kind = PrimitiveKind::Float; break;
        case TokenType::TYPE_DOUBLE: kind = PrimitiveKind::Double; break;
        case TokenType::TYPE_DECIMAL: kind = PrimitiveKind::Decimal; break;
        case TokenType::TYPE_STRING: kind = PrimitiveKind::String; break;
        case TokenType::TYPE_CHAR:   kind = PrimitiveKind::Char; break;
        default:
            ctx.error(stream, DiagCode::E1003, "primitive type", tok.value);
            return nullptr;
    }
    
    auto* type = ctx.arena.make<PrimitiveTypeAST>(kind);
    type->loc = loc;
    
    LOG_PARSER_DETAIL("parsePrimitiveType: parsed primitive type: ", tok.value);
    return type;
}

// =============================================================================
// parseNamedType() - Parse named types and generic param references
// =============================================================================

/**
 * @brief Parse a named type or generic parameter reference.
 * 
 * Grammar: IDENTIFIER [ '<' type { ',' type } '>' ]
 * 
 * This handles both:
 * - User-defined types: `Vec2`, `Player`, `Map<string, int>`
 * - Generic parameter references: `T`, `K`, `V` (in generic contexts)
 * 
 * The semantic pass will distinguish between the two.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return TypeAST* The parsed named type or generic param ref
 */
TypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.error(stream, DiagCode::E1002, "type name", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    Token nameTok = stream.advance();
    InternedString name = ctx.pool.intern(nameTok.value);
    
    // Check if it's a generic parameter reference (single identifier, no generic args)
    // The semantic pass will determine if this is a type parameter or a concrete type
    if (!stream.check(TokenType::LESS)) {
        auto* type = ctx.arena.make<NamedTypeAST>(name);
        type->loc = loc;
        LOG_PARSER_DETAIL("parseNamedType: parsed named type: ", ctx.toString(name));
        return type;
    }
    
    // Parse generic arguments: <type { ',' type }>
    ArenaSpan<TypePtr> genericArgs = parseGenericArgs(stream, ctx);
    
    auto* type = ctx.arena.make<NamedTypeAST>(name);
    type->loc = loc;
    type->genericArgs = genericArgs;
    
    LOG_PARSER_DETAIL("parseNamedType: parsed generic type: ", ctx.toString(name), 
                      " with ", genericArgs.size(), " args");
    return type;
}

// =============================================================================
// parseArrayType() - Parse array types
// =============================================================================

/**
 * @brief Parse an array type.
 * 
 * Grammar: 
 *   array_type = '[' '*' ']' type   -- owned heap array
 *              | '[' '_' ']' type   -- slice (borrowed view)
 *              | '[' INT_LIT ']' type  -- fixed-size stack array
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return ArrayTypeAST* The parsed array type, or nullptr on error
 */
TypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.error(stream, DiagCode::E1004, "[", "array type", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '['
    
    ArrayKind kind;
    uint64_t size = 0;
    
    // Check for dynamic array: [*]
    if (stream.check(TokenType::ARRAY_STAR)) {
        kind = ArrayKind::Dynamic;
        stream.advance(); // Consume '*'
    }
    // Check for slice: [_]
    else if (stream.check(TokenType::ARRAY_UNDER)) {
        kind = ArrayKind::Slice;
        stream.advance(); // Consume '_'
    }
    // Check for fixed-size array: [N]
    else if (stream.check(TokenType::INT_LITERAL)) {
        kind = ArrayKind::Fixed;
        Token sizeTok = stream.advance();
        size = std::stoull(sizeTok.value);
    }
    else {
        ctx.error(stream, DiagCode::E1003, "array size specifier (*, _, or integer)", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        stream.advance(); // Consume ']' to recover
        return nullptr;
    }
    
    // Expect ']'
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.error(stream, DiagCode::E1005, "]", "array type", stream.peekValue());
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (!stream.check(TokenType::RBRACKET)) {
            return nullptr;
        }
    }
    stream.advance(); // Consume ']'
    
    // Parse the element type
    TypePtr element = parseType(stream, ctx);
    if (!element) {
        ctx.error(stream, DiagCode::E1003, "array element type", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    auto* type = ctx.arena.make<ArrayTypeAST>(kind, size, element);
    type->loc = loc;
    
    LOG_PARSER_DETAIL("parseArrayType: parsed array type with element: ", 
                      debug::kindToString(element->kind));
    return type;
}

// =============================================================================
// parseRefType() - Parse reference types
// =============================================================================

/**
 * @brief Parse a reference type.
 * 
 * Grammar: '&' type
 * 
 * References are safe managed references to another value.
 * They are strictly scoped and follow the Downward Flow Rule.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return RefTypeAST* The parsed reference type, or nullptr on error
 */
TypeAST* parseRefType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::AMPERSAND)) {
        ctx.error(stream, DiagCode::E1007, "&", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '&'
    
    TypePtr inner = parseType(stream, ctx);
    if (!inner) {
        ctx.error(stream, DiagCode::E1003, "reference target type", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    auto* type = ctx.arena.make<RefTypeAST>(inner);
    type->loc = loc;
    
    LOG_PARSER_DETAIL("parseRefType: parsed reference type");
    return type;
}

// =============================================================================
// parsePtrType() - Parse pointer types
// =============================================================================

/**
 * @brief Parse a raw pointer type.
 * 
 * Grammar: '*' type
 * 
 * Raw pointers are sealed conduits for FFI and unsafe operations.
 * They cannot be dereferenced directly.
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return PtrTypeAST* The parsed pointer type, or nullptr on error
 */
TypeAST* parsePtrType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    // Check for '*' operator
    if (!stream.check(TokenType::MUL)) {
        ctx.error(stream, DiagCode::E1007, "*", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    stream.advance(); // Consume '*'
    
    // Parse the inner type
    TypePtr inner = parseType(stream, ctx);
    if (!inner) {
        ctx.error(stream, DiagCode::E1003, "pointer target type", stream.peekValue());
        synchronize(stream, ctx);
        return nullptr;
    }
    
    auto* type = ctx.arena.make<PtrTypeAST>(inner);
    type->loc = loc;
    
    LOG_PARSER_DETAIL("parsePtrType: parsed pointer type");
    return type;
}

// =============================================================================
// parseFuncType() - Parse function types
// =============================================================================

/**
 * @brief Parse a function type.
 * 
 * Grammar: '(' [ param_list ] ')' [ '->' return_list ]
 * 
 * Function types represent callable types with parameters and return types.
 * They are recursive - a function type can return another function type.
 * 
 * Return types can be:
 * - A single type: `-> int`
 * - Multiple types in parentheses: `-> (int, string)`
 * - Void (no return): no `->` clause
 * 
 * ## Examples
 * 
 * ```lucid
 * (a int) -> int                    // single return type
 * (a int) -> (int, string)          // multiple return types
 * (a int) -> (b int) -> int         // curried function type
 * (a int)(b int) -> (int, string)   // curried with multiple returns
 * ```
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @return FuncTypeAST* The parsed function type, or nullptr on error
 */
TypeAST* parseFuncType(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseFuncType: start");
    SourceLocation loc = stream.currentLoc();
    
    // ─── 1. Parse parameter list ──────────────────────────────────────────
    std::vector<ParamPtr> params = parseParamList(stream, ctx);
    
    // ─── 2. Create the function type node ────────────────────────────────
    auto* funcType = ctx.arena.make<FuncTypeAST>();
    funcType->loc = loc;
    
    // Build params span
    auto paramBuilder = ctx.arena.makeBuilder<ParamPtr>();
    for (auto* p : params) {
        paramBuilder.push_back(p);
    }
    funcType->params = paramBuilder.build();
    
    // ─── 3. Parse return types (optional) ─────────────────────────────────
    if (stream.match(TokenType::ARROW)) {
        // parseReturnList handles:
        // - Single type: `int`
        // - Multiple types: `(int, string)`
        // - Void: `()` (though this is unusual for function types)
        ArenaSpan<TypePtr> returnTypes = parseReturnList(stream, ctx);
        funcType->returnTypes = returnTypes;
    }
    // If no arrow, it's a void function type (returns nothing)
    
    LOG_PARSER("parseFuncType: parsed function type with ", 
                      params.size(), " parameters and ",
                      funcType->returnTypes.size(), " return types");
    return funcType;
}

// =============================================================================
// parseTypeWithQualifier() - Handle nullable/fallible modifiers
// =============================================================================

/**
 * @brief Parse nullable/fallible modifiers after a type.
 * 
 * Grammar: type = base_type [ '?' ] [ '!' ]  (where '?!' is the only valid order)
 * 
 * Handles:
 * - T?  → nullable
 * - T!  → fallible
 * - T?! → nullable and fallible (combined)
 * 
 * @param stream The token stream
 * @param ctx The parsing context
 * @param inner The inner type to wrap
 * @return TypeAST* The wrapped type, or the original type if no modifiers
 */
TypeAST* parseTypeWithQualifier(TokenStream& stream, ParserContext& ctx, TypePtr inner) {
    if (!inner) {
        return nullptr;
    }
    
    SourceLocation loc = stream.currentLoc();
    bool hasQuestion = stream.match(TokenType::QUESTION);
    bool hasBang = stream.match(TokenType::BANG);
    
    // If no modifiers, return the inner type
    if (!hasQuestion && !hasBang) {
        return inner;
    }
    
    // Handle combined: T?!
    if (hasQuestion && hasBang) {
        auto* type = ctx.arena.make<CombinedTypeAST>(inner);
        type->loc = loc;
        LOG_PARSER_DETAIL("parseTypeWithQualifier: parsed combined type T?!");
        return type;
    }
    
    // Handle nullable: T?
    if (hasQuestion && !hasBang) {
        auto* type = ctx.arena.make<NullableTypeAST>(inner);
        type->loc = loc;
        LOG_PARSER_DETAIL("parseTypeWithQualifier: parsed nullable type T?");
        return type;
    }
    
    // Handle fallible: T!
    if (!hasQuestion && hasBang) {
        auto* type = ctx.arena.make<FallibleTypeAST>(inner);
        type->loc = loc;
        LOG_PARSER_DETAIL("parseTypeWithQualifier: parsed fallible type T!");
        return type;
    }
    
    // Should never reach here
    return inner;
}

} // namespace parser