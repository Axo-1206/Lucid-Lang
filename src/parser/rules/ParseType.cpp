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

#include "../Parser.hpp"
#include "core/Tokens.hpp"
#include "core/ast/BaseAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

namespace parser {

// =============================================================================
// parseType - Main entry point
// =============================================================================

TypeAST* parseType(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseType: parsing type");
    
    TypeAST* type = parseBaseType(stream, ctx);
    if (!type) {
        return nullptr;
    }
    
    return parseTypeWithQualifier(stream, ctx, type);
}

// =============================================================================
// parseBaseType
// =============================================================================

TypeAST* parseBaseType(TokenStream& stream, ParserContext& ctx) {
    LOG_PARSER_DETAIL("parseBaseType: parsing base type");
    
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
    
    // The caller will diagnostic this error based on their context,
    // we do not create diagnostic here
    synchronizeToContext(stream, ctx);
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
        synchronizeToContext(stream, ctx);
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
    type->loc = loc;
    
    LOG_PARSER_DETAIL("parsePrimitiveType: ", tok.value);
    return type;
}

// =============================================================================
// parseNamedType
// =============================================================================

/// @brief Parse a type reference (named or module-qualified).
/// 
/// Grammar:
///   type_reference = IDENTIFIER [ '<' type_arg_list '>' ]           (* unqualified *)
///                   | IDENTIFIER ':' IDENTIFIER [ '<' type_arg_list '>' ]   (* qualified *)
/// 
/// @param stream The token stream
/// @param ctx The parsing context
/// @return TypeAST* - NamedTypeAST or ModuleTypeAccessAST
TypeAST* parseNamedType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::IDENTIFIER)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedIdentifier, stream.currentLoc(),
                                "expected type name, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    Token firstTok = stream.consume();
    InternedString firstName = ctx.pool.intern(firstTok.value);
    
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
        moduleType->loc = loc;
        moduleType->moduleName = firstName;
        moduleType->typeName = typeName;
        moduleType->genericArgs = genericArgs;
        
        LOG_PARSER_DETAIL("parseNamedType: module-qualified '", 
                          ctx.pool.lookup(firstName), ":", ctx.pool.lookup(typeName), "'");
        return moduleType;
    }
    
    // ─── Unqualified type: IDENTIFIER [ '<' type_args '>' ] ──────────
    ArenaSpan<TypeAST*> genericArgs;
    if (stream.check(TokenType::LESS)) {
        genericArgs = parseGenericArgs(stream, ctx);
    }
    
    auto* namedType = ctx.arena.make<NamedTypeAST>(firstName);
    namedType->loc = loc;
    namedType->genericArgs = genericArgs;
    
    LOG_PARSER_DETAIL("parseNamedType: unqualified '", ctx.pool.lookup(firstName), "'");
    return namedType;
}

// =============================================================================
// parseArrayType
// =============================================================================

TypeAST* parseArrayType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::LBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '[', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
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
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '*, '_, or integer for array size, got '", 
                                stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        stream.consume();
        return nullptr;
    }
    
    if (!stream.check(TokenType::RBRACKET)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected ']', got '", stream.peekValue(), "'");
        synchronizeTo(stream, ctx, TokenType::RBRACKET);
        if (!stream.check(TokenType::RBRACKET)) {
            return nullptr;
        }
    }
    stream.consume(); // Consume ']'
    
    TypeAST* element = parseType(stream, ctx);
    if (!element) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected array element type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    auto* type = ctx.arena.make<ArrayTypeAST>(kind, size, element);
    type->loc = loc;
    
    LOG_PARSER_DETAIL("parseArrayType: array of ", debug::kindToString(element->kind));
    return type;
}

// =============================================================================
// parseRefType
// =============================================================================

TypeAST* parseRefType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::AMPERSAND)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '&', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume(); // Consume '&'
    
    TypeAST* inner = parseType(stream, ctx);
    if (!inner) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected reference target type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    auto* type = ctx.arena.make<RefTypeAST>(inner);
    type->loc = loc;
    
    LOG_PARSER_DETAIL("parseRefType: reference to ", debug::typeToString(inner, ctx.pool));
    return type;
}

// =============================================================================
// parsePtrType
// =============================================================================

TypeAST* parsePtrType(TokenStream& stream, ParserContext& ctx) {
    SourceLocation loc = stream.currentLoc();
    
    if (!stream.check(TokenType::MUL)) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedToken, stream.currentLoc(),
                                "expected '*', got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    stream.consume(); // Consume '*'
    
    TypeAST* inner = parseType(stream, ctx);
    if (!inner) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected pointer target type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return nullptr;
    }
    
    // Semantic validation will reject invalid pointer targets
    // (arrays, nullable/fallible, traits, etc.)
    auto* type = ctx.arena.make<PtrTypeAST>(inner);
    type->loc = loc;
    
    LOG_PARSER_DETAIL("parsePtrType: pointer to ", debug::typeToString(inner, ctx.pool));
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
    LOG_PARSER_DETAIL("parseFuncType: start");
    SourceLocation loc = stream.currentLoc();
    
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
    funcType->loc = loc;
    
    auto paramBuilder = ctx.arena.makeBuilder<ParamAST*>();
    for (auto* p : allParams) {
        paramBuilder.push_back(p);
    }
    funcType->params = paramBuilder.build();

    // ─── 3. Check for arrow ─────────────────────────────────────────────────
    if (!stream.check(TokenType::ARROW)) {
        LOG_PARSER_DETAIL("parseFuncType: void function with ", allParams.size(), " params");
        return funcType;
    }
    
    stream.consume(); // Consume '->'
    funcType->hasArrow = true;

    /// NOTE: after '->' we expect a returned type, if no '->' then the function
    ///       is returned before this run
    
    // ─── 4. Parse return type ──────────────────────────────────────────────
    if (stream.check(TokenType::LPAREN)) {
        TypeAST* returnType = parseFuncType(stream, ctx);
        funcType->returnType = returnType;
        LOG_PARSER_DETAIL("parseFuncType: curried function");
        return funcType;
    }
    
    TypeAST* returnType = parseType(stream, ctx);
    if (!returnType) {
        ctx.diagnostics.errorAt(DiagCode::Syntax_ExpectedType, stream.currentLoc(),
                                "expected return type, got '", stream.peekValue(), "'");
        synchronizeToContext(stream, ctx);
        return funcType;
    }
    funcType->returnType = returnType;
    
    LOG_PARSER_DETAIL("parseFuncType: function with ", allParams.size(), " params");
    return funcType;
}

// =============================================================================
// parseTypeWithQualifier
// =============================================================================

TypeAST* parseTypeWithQualifier(TokenStream& stream, ParserContext& ctx, TypeAST* inner) {
    if (!inner) {
        return nullptr;
    }
    
    SourceLocation loc = stream.currentLoc();
    bool hasQuestion = stream.match(TokenType::QUESTION);
    bool hasBang = stream.match(TokenType::BANG);
    
    if (!hasQuestion && !hasBang) {
        return inner;
    }
    
    if (hasQuestion && hasBang) {
        auto* type = ctx.arena.make<CombinedTypeAST>(inner);
        type->loc = loc;
        LOG_PARSER_DETAIL("parseTypeWithQualifier: combined T?!");
        return type;
    }
    
    if (hasQuestion && !hasBang) {
        auto* type = ctx.arena.make<NullableTypeAST>(inner);
        type->loc = loc;
        LOG_PARSER_DETAIL("parseTypeWithQualifier: nullable T?");
        return type;
    }
    
    // hasBang && !hasQuestion
    auto* type = ctx.arena.make<FallibleTypeAST>(inner);
    type->loc = loc;
    LOG_PARSER_DETAIL("parseTypeWithQualifier: fallible T!");
    return type;
}

} // namespace parser