/**
 * @file ParserType.cpp
 * 
 * Parses all type annotations and signatures.
 */

#include "Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

// -----------------------------------------------------------------------------
// parseType - root entry point
// -----------------------------------------------------------------------------

TypePtr Parser::parseType() {
    return parseTypeWithNullable();
}

// -----------------------------------------------------------------------------
// parseTypeWithNullable - type + optional '?'
// -----------------------------------------------------------------------------

TypePtr Parser::parseTypeWithNullable() {
    TypePtr ty = parseBaseType();
    if (ty && ts_.match(TokenType::QUESTION)) {
        ty = arena_.make<NullableTypeAST>(std::move(ty));
    }
    return ty;
}

// -----------------------------------------------------------------------------
// parseBaseType - dispatch to concrete type parsers
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// parsePrimitiveType
// -----------------------------------------------------------------------------

TypePtr Parser::parsePrimitiveType() {
    SourceLocation loc = ts_.currentLoc();
    Token tok = ts_.advance();

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
        case TokenType::TYPE_ANY:    kind = PrimitiveKind::Any; break;
        default:
            errorAt(DiagCode::E2002, "internal error: expected primitive type");
            return arena_.make<UnknownTypeAST>();
    }

    auto node = arena_.make<PrimitiveTypeAST>(kind);
    node->loc = loc;
    return node;
}

// -----------------------------------------------------------------------------
// parseNamedType
// -----------------------------------------------------------------------------

TypePtr Parser::parseNamedType() {
    SourceLocation loc = ts_.currentLoc();
    
    if (!ts_.check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected type name");
        return arena_.make<UnknownTypeAST>();
    }
    
    InternedString name = pool_.intern(ts_.advance().value);
    auto node = arena_.make<NamedTypeAST>(name);
    node->loc = loc;

    if (ts_.check(TokenType::LESS)) {
        node->genericArgs = parseGenericArgs();
    }

    return node;
}

// -----------------------------------------------------------------------------
// parseArrayType
// -----------------------------------------------------------------------------

TypePtr Parser::parseArrayType() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::LBRACKET, "expected '['");

    // Dynamic array: [*]T
    if (ts_.check(TokenType::MUL)) {
        ts_.advance();
        ts_.consume(TokenType::RBRACKET, "expected ']' after '*'");
        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[*]'");
            return arena_.make<UnknownTypeAST>();
        }
        auto node = arena_.make<DynamicArrayTypeAST>(std::move(elem));
        node->loc = loc;
        return node;
    }

    // Slice: []T
    if (ts_.check(TokenType::RBRACKET)) {
        ts_.advance();
        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[]'");
            return arena_.make<UnknownTypeAST>();
        }
        auto node = arena_.make<SliceTypeAST>(std::move(elem));
        node->loc = loc;
        return node;
    }

    // Fixed array: [N]T
    if (ts_.check(TokenType::INT_LITERAL)) {
        Token sizeTok = ts_.advance();
        std::string raw = sizeTok.value;
        raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());
        
        char* end = nullptr;
        uint64_t size = std::strtoull(raw.c_str(), &end, 10);
        if (*end != '\0') {
            error(ts_.locOf(sizeTok), DiagCode::E2009, "invalid array size");
            size = 0;
        }
        
        ts_.consume(TokenType::RBRACKET, "expected ']' after array size");
        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[" + sizeTok.value + "]'");
            return arena_.make<UnknownTypeAST>();
        }
        auto node = arena_.make<FixedArrayTypeAST>(size, std::move(elem));
        node->loc = loc;
        return node;
    }

    errorAt(DiagCode::E2001, "expected ']', '*', or integer in array type");
    // Recovery: skip to matching ']'
    int depth = 1;
    while (!ts_.isAtEnd() && depth > 0) {
        if (ts_.check(TokenType::LBRACKET)) depth++;
        else if (ts_.check(TokenType::RBRACKET)) depth--;
        ts_.advance();
    }
    return arena_.make<UnknownTypeAST>();
}

// -----------------------------------------------------------------------------
// parseRefType
// -----------------------------------------------------------------------------

TypePtr Parser::parseRefType() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::AMPERSAND, "expected '&'");
    TypePtr inner = parseBaseType();
    if (!inner) {
        errorAt(DiagCode::E2005, "expected type after '&'");
        return arena_.make<UnknownTypeAST>();
    }
    auto node = arena_.make<RefTypeAST>(std::move(inner));
    node->loc = loc;
    return node;
}

// -----------------------------------------------------------------------------
// parsePtrType
// -----------------------------------------------------------------------------

TypePtr Parser::parsePtrType() {
    SourceLocation loc = ts_.currentLoc();
    ts_.consume(TokenType::MUL, "expected '*'");
    TypePtr inner = parseBaseType();
    if (!inner) {
        errorAt(DiagCode::E2005, "expected type after '*'");
        return arena_.make<UnknownTypeAST>();
    }
    auto node = arena_.make<PtrTypeAST>(std::move(inner));
    node->loc = loc;
    return node;
}

// -----------------------------------------------------------------------------
// parseFuncType
// -----------------------------------------------------------------------------

TypePtr Parser::parseFuncType() {
    SourceLocation loc = ts_.currentLoc();
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Raw qualifiers
    std::vector<InternedString> rawQuals;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        rawQuals.push_back(pool_.intern(ts_.advance().value));
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->sig.rawQualifiers = qBuilder.build();

    // Parameter groups: flat accumulation
    std::vector<ParamPtr> allParams;
    std::vector<size_t> groupSizes;
    
    if (!ts_.check(TokenType::LPAREN)) {
        errorAt(DiagCode::E2001, "expected '(' for function type parameters");
        return arena_.make<UnknownTypeAST>();
    }
    
    while (ts_.check(TokenType::LPAREN)) {
        ParamGroup group = parseParamGroup();
        groupSizes.push_back(group.size());
        for (size_t i = 0; i < group.size(); ++i) {
            allParams.push_back(std::move(const_cast<ParamPtr&>(group[i])));
        }
    }
    
    auto paramsBuilder = arena_.makeBuilder<ParamPtr>();
    for (auto& p : allParams) paramsBuilder.push_back(std::move(p));
    funcType->sig.allParams = paramsBuilder.build();
    
    auto gsBuilder = arena_.makeBuilder<size_t>();
    for (auto& sz : groupSizes) gsBuilder.push_back(sz);
    funcType->sig.groupSizes = gsBuilder.build();

    // Return types
    if (ts_.match(TokenType::ARROW)) {
        funcType->sig.returnTypes = parseReturnList();
    }

    return funcType;
}

// -----------------------------------------------------------------------------
// parseGenericArgs
// -----------------------------------------------------------------------------

ArenaSpan<TypePtr> Parser::parseGenericArgs() {
    if (!ts_.match(TokenType::LESS)) return ArenaSpan<TypePtr>();
    
    if (ts_.check(TokenType::GREATER)) {
        ts_.advance();
        return ArenaSpan<TypePtr>();
    }
    
    std::vector<TypePtr> args;
    do {
        if (ts_.match(TokenType::COMMA)) continue;
        
        size_t savedPos = ts_.getPos();
        TypePtr arg = parseType();
        if (ts_.getPos() == savedPos) {
            errorAt(DiagCode::E2005, "expected type in generic argument list");
            if (!ts_.isAtEnd()) ts_.advance();
            break;
        }
        args.push_back(std::move(arg));
    } while (!ts_.check(TokenType::GREATER) && !ts_.isAtEnd());
    
    ts_.consume(TokenType::GREATER, "expected '>' to close generic arguments");
    
    auto builder = arena_.makeBuilder<TypePtr>();
    for (auto& a : args) builder.push_back(std::move(a));
    return builder.build();
}