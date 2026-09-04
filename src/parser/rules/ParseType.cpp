/// @file ParseType.cpp
/// @brief Implementation of type parsers.
/// 
/// This file implements all type parsing functions:
/// - Primitive, Named, Array, Reference, Pointer, Function types
/// - Type with nullable/fallible modifiers (T?, T!, T?!)
/// 
/// The type parser uses a Pratt-style recursive descent approach.
/// 
/// NOTE: FutureTypeAST and ThreadTypeAST is used directly by parseAsyncStmt
///  and parseSpawnStmt in ParseStmt.cpp, reason: if we implement 2 functions
///  to parse them here then it will require us to move backward instead of 
///  forward, for example a nullable and fallible type T? and T! is easy to tell
///  the parser only need to look at the next token, but for future and thread
///  the problem is we need to look backward and find the token async/spawn before
///  the declaration keyword, this sounds trivial until we realized that parameter
///  in function and generic paramter do not allow async/spawn (and let/const keywords).
///  so it's easier and less bugs that we warp the node directly in parseAsyncStmt and
///  parseSpawnStmt instead of implement their own parser here
/// 
/// NOTE: Do not synchronize or attempt to error handling when we attempt to parse a
///  type, because we lack of context at declaration site, we may skip essential token
///  like '=' or ';' or '}'

#include "../Parser.hpp"
#include "core/SourceLocation.hpp"
#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"

namespace parser {

// =============================================================================
// parseType - Main entry point
// =============================================================================

TypeAST* parseType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();

    TypeAST* type = parseBaseType(stream, ctx);
    if (!type) {
        return nullptr;
    }
    type->loc = loc;
    
    return parseTypeWithQualifier(stream, ctx, type);
}

// =============================================================================
// parseBaseType
// =============================================================================

TypeAST* parseBaseType(TokenStream& stream, ParserContext& ctx) {
    
    if (stream.isPrimitiveTypeToken(stream.peekType())) {
        return parsePrimitiveType(stream, ctx);
    }
    
    if (stream.check(TokenType::LBRACKET)) {
        return parseArrayType(stream, ctx);
    }
    
    if (stream.check(TokenType::AMPERSAND)) {
        return parseRefType(stream, ctx);
    }
    
    if (stream.check(TokenType::MUL)) {
        return parsePtrType(stream, ctx);
    }
    
    if (stream.check(TokenType::LPAREN)) {
        return parseFuncType(stream, ctx);
    }
    
    if (stream.check(TokenType::IDENTIFIER)) {
        // parseNamedType handles both unqualified and module-qualified types
        return parseNamedType(stream, ctx);
    }
    
    // The caller will diagnostic and synchronize to its context
    // this error based on their context, we do not create diagnostic here
    return nullptr;
}

// =============================================================================
// parsePrimitiveType
// =============================================================================

TypeAST* parsePrimitiveType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.isPrimitiveTypeToken(stream.peekType())) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected primitive type, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    Token tok = stream.consume();
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
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, loc,
                                    "unknown primitive type '", tok.value, "'");
            return nullptr;
    }
    
    auto* type = ctx.arena.make<PrimitiveTypeAST>(kind);
    
    return type;
}

// =============================================================================
// parseNamedType
// =============================================================================

/// @brief Check if a type is a valid Simd element type.
/// 
/// Valid types are numeric primitives only:
///   - Signed: int8, int16, int32, int64
///   - Unsigned: uint8, uint16, uint32, uint64
///   - Floating: float32, float64
static bool isValidSimdElementType(TypeAST* type) {
    if (!type) return false;
    
    // Must be a primitive type
    if (type->kind != ASTKind::PrimitiveType) {
        return false;
    }
    
    auto* prim = static_cast<PrimitiveTypeAST*>(type);
    PrimitiveKind kind = prim->primitiveKind;
    
    // Check against the allowed set
    switch (kind) {
        // Signed integers
        case PrimitiveKind::Int8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int64:
        // Unsigned integers
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint64:
        // Floating point
        case PrimitiveKind::Float:
        case PrimitiveKind::Double:
            return true;
        default:
            return false;
    }
}

/// @brief Parse a type reference (named, module-qualified, or built-in).
/// 
/// Grammar:
///   type_reference = IDENTIFIER [ '<' type_arg_list '>' ]           (* unqualified *)
///                   | IDENTIFIER ':' IDENTIFIER [ '<' type_arg_list '>' ]   (* qualified *)
/// 
/// Built-in types are handled specially:
///   - Simd<T, N>  → SimdTypeAST with element type and lane count
///   - Arena       → ArenaTypeAST (no generic args allowed)
///   - ArenaDescriptor → ArenaDescriptorTypeAST (no generic args allowed)
/// 
/// @param stream The token stream
/// @param ctx The parsing context
/// @return TypeAST* - NamedTypeAST, ModuleTypeAccessAST, or built-in type node
TypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx) {
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected type name, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    Token firstTok = stream.consume();
    SourceLocation firstLoc = stream.currentLoc();
    InternedString firstName = ctx.pool.intern(firstTok.value);
    
    // ─── Check for built-in types FIRST ────────────────────────────────
    // These are special types that have their own AST nodes.
    // They must be checked before module qualification because they are
    // always unqualified (you can't write "mymod:Arena").
    
    // ─── Simd<T, N> ─────────────────────────────────────────────────────
    if (firstName == ctx.pool.intern("Simd")) {
        // Must have generic arguments
        if (!stream.check(TokenType::LESS)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '<' after 'Simd'");
            return nullptr;
        }
        
        stream.consume(); // Consume '<'
        
        // ─── Parse element type ──────────────────────────────────────────
        TypeAST* elementType = parseType(stream, ctx);
        if (!elementType) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                    "expected Simd element type");
            return nullptr;
        }
        
        // ─── Parse comma ─────────────────────────────────────────────────
        if (!stream.match(TokenType::COMMA)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected ',' between Simd arguments");
            return nullptr;
        }
        
        // ─── Parse lane count (must be integer literal) ─────────────────
        if (!stream.check(TokenType::INT_LITERAL)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedLiteral, stream.currentLoc(),
                                    "Simd lane count must be an integer literal");
            return nullptr;
        }
        
        Token laneTok = stream.consume();
        uint64_t laneCount;
        try {
            laneCount = std::stoull(laneTok.value);
        } catch (const std::exception&) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidSimdLaneCount, stream.currentLoc(),
                                    "invalid integer literal for Simd lane count: '", laneTok.value, "'");
            return nullptr;
        }
        
        // ─── Parse closing '>' ───────────────────────────────────────────
        if (!stream.match(TokenType::GREATER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "expected '>' after Simd arguments");
            return nullptr;
        }
        
        // ─── Create the Simd node ────────────────────────────────────────
        auto* simdType = ctx.arena.make<SimdTypeAST>(elementType, laneCount);
        simdType->loc = firstLoc;
        return simdType;
    }
    
    // ─── Arena ──────────────────────────────────────────────────────────
    if (firstName == ctx.pool.intern("Arena")) {
        // Arena has no generic arguments
        if (stream.check(TokenType::LESS)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "unexpected '<' after 'Arena'");
            // Skip to matching '>' for error recovery
            int depth = 1;
            while (!stream.isAtEnd() && depth > 0) {
                if (stream.match(TokenType::LESS)) depth++;
                else if (stream.match(TokenType::GREATER)) depth--;
                else stream.consume();
            }
            return nullptr;
        }
        
        auto* arenaType = ctx.arena.make<ArenaTypeAST>();
        arenaType->loc = firstLoc;
        return arenaType;
    }
    
    // ─── ArenaDescriptor ────────────────────────────────────────────────
    if (firstName == ctx.pool.intern("ArenaDescriptor")) {
        // ArenaDescriptor has no generic arguments
        if (stream.check(TokenType::LESS)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                    "unexpected '<' after 'ArenaDescriptor'");
            // Skip to matching '>' for error recovery
            int depth = 1;
            while (!stream.isAtEnd() && depth > 0) {
                if (stream.match(TokenType::LESS)) depth++;
                else if (stream.match(TokenType::GREATER)) depth--;
                else stream.consume();
            }
            return nullptr;
        }
        
        auto* descType = ctx.arena.make<ArenaDescriptorTypeAST>();
        descType->loc = firstLoc;
        return descType;
    }
    
    // ─── Check for module qualification: IDENTIFIER ':' IDENTIFIER ────
    if (stream.check(TokenType::COLON)) {
        stream.consume(); // Consume ':'
        
        if (!stream.check(TokenType::IDENTIFIER)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                    "expected type name after ':', got '", stream.peekValue(), "'");
            return nullptr;
        }
        
        Token secondTok = stream.consume();
        InternedString typeName = ctx.pool.intern(secondTok.value);
        
        ArenaSpan<TypeAST*> genericArgs;
        if (stream.check(TokenType::LESS)) {
            genericArgs = parseGenericArgs(stream, ctx);
        }
        
        auto* moduleType = ctx.arena.make<ModuleTypeAccessAST>();
        moduleType->moduleName = firstName;
        moduleType->typeName = typeName;
        moduleType->genericArgs = genericArgs;
        moduleType->loc = firstLoc;
        
        return moduleType;
    }
    
    // ─── Unqualified type: IDENTIFIER [ '<' type_args '>' ] ──────────
    ArenaSpan<TypeAST*> genericArgs;
    if (stream.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs(stream, ctx);
    }
    
    auto* namedType = ctx.arena.make<NamedTypeAST>(firstName);
    namedType->genericArgs = genericArgs;
    namedType->loc = firstLoc;
    
    return namedType;
}

// =============================================================================
// parseArrayType
// =============================================================================

TypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx) {
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '[', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume(); // Consume '['
    
    ArrayKind kind;
    uint64_t size = 0;
    
    if (stream.check(TokenType::ARRAY_STAR)) {
        kind = ArrayKind::Dynamic;
        stream.consume();
    } else if (stream.check(TokenType::ARRAY_UNDER)) {
        kind = ArrayKind::Slice;
        stream.consume();
    } else if (stream.check(TokenType::INT_LITERAL)) {
        kind = ArrayKind::Fixed;
        Token sizeTok = stream.consume();
        size = std::stoull(sizeTok.value);
    } else {
        if (stream.check(TokenType::RBRACKET)) {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                        "expected '*, '_, or integer for array size, but found none");
            // The code below will consume ']' for us
        } else {
            ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                        "expected '*, '_, or integer for array size, got '", 
                        stream.peekValue(), "'");

            stream.consume();
        }
        // Continue parse the type if possible
    }
    
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']', got '", stream.peekValue(), "'");
        if (!stream.check(TokenType::RBRACKET)) {
            return nullptr;
        }
    }
    stream.consume(); // Consume ']'
    
    TypeAST* element = parseType(stream, ctx);
    if (!element) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected array element type, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    auto* type = ctx.arena.make<ArrayTypeAST>(kind, size, element);
    
    return type;
}

// =============================================================================
// parseRefType
// =============================================================================

TypeAST* parseRefType(TokenStream& stream, ParserContext& ctx) {
    if (!stream.check(TokenType::AMPERSAND)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '&', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume(); // Consume '&'
    
    TypeAST* inner = parseType(stream, ctx);
    if (!inner) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected reference target type, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    auto* type = ctx.arena.make<RefTypeAST>(inner);
    
    return type;
}

// =============================================================================
// parsePtrType
// =============================================================================

TypeAST* parsePtrType(TokenStream& stream, ParserContext& ctx) {
    if (!stream.check(TokenType::MUL)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '*', got '", stream.peekValue(), "'");
        return nullptr;
    }
    stream.consume(); // Consume '*'
    
    TypeAST* inner = parseType(stream, ctx);
    if (!inner) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected pointer target type, got '", stream.peekValue(), "'");
        return nullptr;
    }
    
    // Semantic validation will reject invalid pointer targets
    // (arrays, nullable/fallible, traits, etc.)
    auto* type = ctx.arena.make<PtrTypeAST>(inner);
    
    return type;
}

// =============================================================================
// parseFuncType - Parses function types with adjacent groups
// =============================================================================

/// @brief Parse a function type.
///
/// Grammar:
///   func_type = unnamed_cluster { [ '->' ] unnamed_cluster } [ '->' type ]
///
/// Parameter names are NEVER allowed in a function type.
///
/// @param stream The token stream
/// @param ctx The parsing context
/// @return TypeAST* The parsed function type
TypeAST* parseFuncType(TokenStream& stream, ParserContext& ctx) {
    // ─── 1. Parse all parameter groups before the first `->` ──────────────
    // Parameter names are NEVER allowed in a function type.
    std::vector<ParamAST*> allParams;
    
    while (stream.check(TokenType::LPAREN)) {
        // Parse a single parameter group with allowNames = false
        std::vector<ParamAST*> groupParams = parseParamList(stream, ctx, false);
        for (auto* p : groupParams) {
            allParams.push_back(p);
        }
        
        if (stream.check(TokenType::ARROW)) {
            break;
        }
    }
    
    // ─── 2. Create function type node ──────────────────────────────────────
    auto* funcType = ctx.arena.make<FuncTypeAST>();

    auto paramBuilder = ctx.arena.makeBuilder<ParamAST*>();
    for (auto* p : allParams) {
        paramBuilder.push_back(p);
    }
    funcType->params = paramBuilder.build();

    // ─── 3. Check for arrow ─────────────────────────────────────────────────
    if (!stream.match(TokenType::ARROW)) {
        return funcType;
    }
    /// NOTE: after '->' we expect a returned type, if no '->' then the function
    ///       is returned before this run
    
    // ─── 4. Parse return type ──────────────────────────────────────────────
    TypeAST* returnType = parseType(stream, ctx);
    if (!returnType) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected return type, got '", stream.peekValue(), "'");
        return funcType;
    }
    funcType->returnType = returnType;
    
    return funcType;
}

// =============================================================================
// parseTypeWithQualifier
// =============================================================================

TypeAST* parseTypeWithQualifier(TokenStream& stream, ParserContext& ctx, TypeAST* inner) {
    if (!inner) {
        return nullptr;
    }
    
    bool hasQuestion = stream.match(TokenType::QUESTION);
    bool hasBang = stream.match(TokenType::BANG);
    
    if (!hasQuestion && !hasBang) {
        return inner;
    }
    
    if (hasQuestion && hasBang) {
        auto* type = ctx.arena.make<CombinedTypeAST>(inner);
        return type;
    }
    
    if (hasQuestion && !hasBang) {
        auto* type = ctx.arena.make<NullableTypeAST>(inner);
        return type;
    }
    
    // hasBang && !hasQuestion
    auto* type = ctx.arena.make<FallibleTypeAST>(inner);
    return type;
}

} // namespace parser