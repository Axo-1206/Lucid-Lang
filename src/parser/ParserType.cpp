/**
 * @file ParserType.cpp
 *
 * @responsibility Parses type annotations and signatures.
 *
 * @grammar_rules Primitive types, Union types (|), Nullable (?), Generics (<>), Functions.
 *
 * @related src/diagnostics/DiagnosticEngine.hpp, DiagnosticCodes.hpp
 */

#include "Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib> // std::strtoull
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ParserType.cpp
//
// Implements every type-annotation parse function declared in Parser.hpp.
// No expression or statement parsing happens here — this file is purely about
// turning a token stream position that starts a type annotation into the
// correct TypeAST subtree.
//
// Entry point:  TypePtr parseType()
//
// Call hierarchy:
//
//   parseType()               — union chain:  T { '|' T }
//     └─ parseBaseType()      — single non-union type
//          ├─ parsePrimitiveType()
//          ├─ parseNamedType()     — IDENTIFIER [ '<' generic_args '>' ]
//          ├─ parseArrayType()     — '[' ...  →  fixed / slice / dynamic
//          ├─ parseRefType()       — '&' T
//          ├─ parsePtrType()       — '*' T
//          └─ parseFuncType()      — '(' params ')' [ ret ] [ '?' ]
//
//   wrapNullable(inner)       — wraps any TypePtr in NullableTypeAST when '?' follows
//   parseGenericArgs()        — '<' type { ',' type } '>'
//
// Grammar source: LUC_GRAMMAR.md §Types
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseType  — root entry point
//
// Grammar:
//   type := base_type [ '?' ]
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parseType() {
    LUC_LOG_TYPE_VERBOSE("=== parseType START ===");
    LUC_LOG_TYPE("parseType: starting at token '" << peek().value << "', type = " << LucDebug::tokenTypeToString(peek().type));

    TypePtr result = parseBaseType();

    if (result) {
        LUC_LOG_TYPE("parseType: result kind = " << LucDebug::kindToString(result->kind));
        LUC_LOG_TYPE_VERBOSE("=== parseType END (success) ===");
    } else {
        LUC_LOG_TYPE("parseType: FAILED to parse type");
        LUC_LOG_TYPE_VERBOSE("=== parseType END (failure) ===");
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseBaseType  — single type
//
// Dispatches on the current token to the specific sub-parser.  Each sub-parser
// is responsible for consuming the tokens that make up its form and calling
// wrapNullable() on its result before returning.
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parseBaseType() {
    LUC_LOG_TYPE_VERBOSE("parseBaseType: current token = '" << peek().value
                         << "', type = " << LucDebug::tokenTypeToString(peek().type));

    switch (peek().type) {

        // ── Primitive keywords ────────────────────────────────────────────────
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
            LUC_LOG_TYPE_VERBOSE("parseBaseType: dispatching to parsePrimitiveType");
            return parsePrimitiveType();

        // ── Named (user-defined) type ─────────────────────────────────────────
        case TokenType::IDENTIFIER:
            LUC_LOG_TYPE_VERBOSE("parseBaseType: dispatching to parseNamedType");
            return parseNamedType();

        // ── Array types  [N]T  /  []T  /  [*]T ───────────────────────────────
        case TokenType::LBRACKET:
            LUC_LOG_TYPE_VERBOSE("parseBaseType: dispatching to parseArrayType");
            return parseArrayType();

        // ── Reference  &T ─────────────────────────────────────────────────────
        case TokenType::AMPERSAND:
            LUC_LOG_TYPE_VERBOSE("parseBaseType: dispatching to parseRefType (reference &T)");
            return wrapNullable(parseRefType());

        // ── Raw pointer  *T  (extern/FFI only) ────────────────────────────────
        case TokenType::MUL:
            LUC_LOG_TYPE_VERBOSE("parseBaseType: dispatching to parsePtrType (raw pointer *T)");
            return wrapNullable(parsePtrType());

        // ── Function type  '(' params ')' [ ret ] ─────────────────────────────
        // A '(' in type position always starts a function type.  A grouped
        // expression would only appear in expression context, never here.
        case TokenType::LPAREN:
            LUC_LOG_TYPE_VERBOSE("parseBaseType: dispatching to parseFuncType");
            return parseFuncType(true);

        default:
            // Not a recognisable type start — caller decides if that is an error.
            LUC_LOG_TYPE("parseBaseType: unrecognized type start: '" << peek().value << "'");
            return arena_.make<UnknownTypeAST>();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePrimitiveType
//
// Grammar:
//   primitive_type := 'bool' | 'byte' | 'short' | 'int' | 'long'
//                   | 'ubyte' | 'ushort' | 'uint' | 'ulong'
//                   | 'int8' | 'int16' | 'int32' | 'int64'
//                   | 'uint8' | 'uint16' | 'uint32' | 'uint64'
//                   | 'float' | 'double' | 'decimal'
//                   | 'string' | 'char' | 'any'
//
// Maps each keyword token to its PrimitiveKind and calls wrapNullable().
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parsePrimitiveType() {
    LUC_LOG_TYPE_VERBOSE("=== parsePrimitiveType START ===");
    SourceLocation loc = currentLoc();
    Token tok = advance();
    LUC_LOG_TYPE("parsePrimitiveType: consuming primitive token '" << tok.value << "'");

    PrimitiveKind kind;
    switch (tok.type) {
        case TokenType::TYPE_BOOL:
            kind = PrimitiveKind::Bool;
            break;
        case TokenType::TYPE_BYTE:
            kind = PrimitiveKind::Byte;
            break;
        case TokenType::TYPE_SHORT:
            kind = PrimitiveKind::Short;
            break;
        case TokenType::TYPE_INT:
            kind = PrimitiveKind::Int;
            break;
        case TokenType::TYPE_LONG:
            kind = PrimitiveKind::Long;
            break;
        case TokenType::TYPE_UBYTE:
            kind = PrimitiveKind::Ubyte;
            break;
        case TokenType::TYPE_USHORT:
            kind = PrimitiveKind::Ushort;
            break;
        case TokenType::TYPE_UINT:
            kind = PrimitiveKind::Uint;
            break;
        case TokenType::TYPE_ULONG:
            kind = PrimitiveKind::Ulong;
            break;
        case TokenType::TYPE_INT8:
            kind = PrimitiveKind::Int8;
            break;
        case TokenType::TYPE_INT16:
            kind = PrimitiveKind::Int16;
            break;
        case TokenType::TYPE_INT32:
            kind = PrimitiveKind::Int32;
            break;
        case TokenType::TYPE_INT64:
            kind = PrimitiveKind::Int64;
            break;
        case TokenType::TYPE_UINT8:
            kind = PrimitiveKind::Uint8;
            break;
        case TokenType::TYPE_UINT16:
            kind = PrimitiveKind::Uint16;
            break;
        case TokenType::TYPE_UINT32:
            kind = PrimitiveKind::Uint32;
            break;
        case TokenType::TYPE_UINT64:
            kind = PrimitiveKind::Uint64;
            break;
        case TokenType::TYPE_FLOAT:
            kind = PrimitiveKind::Float;
            break;
        case TokenType::TYPE_DOUBLE:
            kind = PrimitiveKind::Double;
            break;
        case TokenType::TYPE_DECIMAL:
            kind = PrimitiveKind::Decimal;
            break;
        case TokenType::TYPE_STRING:
            kind = PrimitiveKind::String;
            break;
        case TokenType::TYPE_CHAR:
            kind = PrimitiveKind::Char;
            break;
        case TokenType::TYPE_ANY:
            kind = PrimitiveKind::Any;
            break;

        default:
            // Should never reach here — parseBaseType only calls us on known
            // primitive tokens.
            errorAt(DiagCode::E2002, "internal error: parsePrimitiveType called on non-primitive token");
            return arena_.make<UnknownTypeAST>();
    }

    auto node = arena_.make<PrimitiveTypeAST>(kind);
    node->loc = loc;
    LUC_LOG_TYPE_VERBOSE("parsePrimitiveType: returning PrimitiveTypeAST for '" << tok.value << "'");
    return wrapNullable(std::move(node));
}

// ─────────────────────────────────────────────────────────────────────────────
// parseNamedType
//
// Grammar:
//   named_type := IDENTIFIER [ '<' type { [','] type } '>' ] [ '?' ]
//
// Handles both simple named types (Vec2) and generic instantiations (Buffer<int>).
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parseNamedType() {
    LUC_LOG_TYPE_VERBOSE("=== parseNamedType START ===");
    SourceLocation loc = currentLoc();
    std::string name = advance().value;
    LUC_LOG_TYPE("parseNamedType: name = '" << name << "'");

    auto node = arena_.make<NamedTypeAST>(std::move(pool_.intern(name)));
    node->loc = loc;

    // Optional generic argument list: '<' type { ',' type } '>'
    if (check(TokenType::LESS)) {
        LUC_LOG_TYPE("parseNamedType: parsing generic arguments for '" << pool_.lookup(node->name) << "'");
        node->genericArgs = parseGenericArgs();
        LUC_LOG_TYPE("parseNamedType: parsed " << node->genericArgs.size() << " generic argument(s) for '" << pool_.lookup(node->name) << "'");
    }

    LUC_LOG_TYPE_VERBOSE("parseNamedType: returning NamedTypeAST for '" << pool_.lookup(node->name) << "'");
    return wrapNullable(std::move(node));
}

// ─────────────────────────────────────────────────────────────────────────────
// parseArrayType
//
// Grammar:
//   array_type := '[' INT_LITERAL ']' type      — fixed:   [100]int
//              |  '[' ']' type                  — slice:   []int
//              |  '[' '*' ']' type              — dynamic: [*]int
//
// Multidimensional arrays are handled naturally because the element type is
// parsed via parseType() which will recursively call parseArrayType() again
// if the element itself starts with '['.
//
// Examples:
//   [4]float     →  FixedArrayTypeAST { size=4, element=Float }
//   []int        →  SliceTypeAST      { element=Int }
//   [*]Vec2      →  DynamicArrayTypeAST { element=Vec2 }
//   [][*]float   →  SliceTypeAST      { element=DynamicArrayTypeAST{Float} }
//   [4][4]float  →  FixedArrayTypeAST { size=4, element=FixedArrayTypeAST{4,Float} }
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parseArrayType() {
    LUC_LOG_TYPE_VERBOSE("=== parseArrayType START ===");
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACKET, "expected '['");

    // ── [*]T  —  dynamic array ────────────────────────────────────────────────
    if (check(TokenType::MUL)) {
        LUC_LOG_TYPE("parseArrayType: dynamic array '[*]T'");
        advance(); // consume '*'
        consume(TokenType::RBRACKET, DiagCode::E2001, "expected ']' after '*' in dynamic array type");

        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[*]'");
            LUC_LOG_TYPE_VERBOSE("=== parseArrayType END (error: no element type after [*]) ===");
            return arena_.make<UnknownTypeAST>();
        }

        auto node = arena_.make<DynamicArrayTypeAST>(std::move(elem));
        node->loc = loc;
        // Dynamic array types are not nullable by themselves — wrapNullable
        // would produce [*]T? which is a nullable dynamic array, a valid form.
        LUC_LOG_TYPE_VERBOSE("parseArrayType: returning DynamicArrayTypeAST");
        return wrapNullable(std::move(node));
    }

    // ── []T  — slice ──────────────────────────────────────────────────────────
    if (check(TokenType::RBRACKET)) {
        LUC_LOG_TYPE("parseArrayType: slice '[]T'");
        advance(); // consume ']'

        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[]'");
            LUC_LOG_TYPE_VERBOSE("=== parseArrayType END (error: no element type after []) ===");
            return arena_.make<UnknownTypeAST>();
        }

        auto node = arena_.make<SliceTypeAST>(std::move(elem));
        node->loc = loc;
        LUC_LOG_TYPE_VERBOSE("parseArrayType: returning SliceTypeAST");
        return wrapNullable(std::move(node));
    }

    // ── [N]T  — fixed array ───────────────────────────────────────────────────
    if (check(TokenType::INT_LITERAL)) {
        LUC_LOG_TYPE("parseArrayType: fixed array '[N]T'");
        Token sizeTok = advance();

        // Parse the integer literal. The Lexer guarantees only decimal digits
        // (and optional '_' separators stripped by the Lexer — actually not
        // stripped: the Lexer keeps them in the value, so strip here).
        std::string raw = sizeTok.value;
        // Remove '_' separators that the grammar allows for readability.
        raw.erase(std::remove(raw.begin(), raw.end(), '_'), raw.end());

        char *end = nullptr;
        std::uint64_t size = std::strtoull(raw.c_str(), &end, 10);
        if (*end != '\0' || size == ~0ULL) {
            error(locOf(sizeTok), DiagCode::E2002,
                  "array size '" + sizeTok.value + "' is not a valid integer");
        }

        consume(TokenType::RBRACKET, DiagCode::E2001, "expected ']' after array size");

        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[" + sizeTok.value + "]'");
            LUC_LOG_TYPE_VERBOSE("=== parseArrayType END (error: no element type after [N]) ===");
            return arena_.make<UnknownTypeAST>();
        }

        auto node = arena_.make<FixedArrayTypeAST>(size, std::move(elem));
        node->loc = loc;
        LUC_LOG_TYPE_VERBOSE("parseArrayType: returning FixedArrayTypeAST (size=" << size << ")");
        return wrapNullable(std::move(node));
    }

    // ── Unrecognised content between '[' and the next token ───────────────────
    LUC_LOG_TYPE("parseArrayType: ERROR - unrecognized array syntax, token = '" << peek().value << "'");
    errorAt(DiagCode::E2001, "expected ']', '*', or an integer literal inside array type brackets");
    // Best-effort recovery: skip to the matching ']' if possible.
    int depth = 1;
    while (!isAtEnd() && depth > 0) {
        if (check(TokenType::LBRACKET))
            ++depth;
        else if (check(TokenType::RBRACKET))
            --depth;
        advance();
    }
    LUC_LOG_TYPE_VERBOSE("=== parseArrayType END (error) ===");
    return arena_.make<UnknownTypeAST>();
}

// ─────────────────────────────────────────────────────────────────────────────
// parseRefType
//
// Grammar:  ref_type := '&' type
//
// Produces:  RefTypeAST { inner = parseType() }
//
// A '?' after the inner type is handled by wrapNullable inside parseType(),
// so  &Vec2?  parses as  RefTypeAST{ NullableTypeAST{ Vec2 } }  which means
// "a reference to a nullable Vec2".
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parseRefType() {
    LUC_LOG_TYPE_VERBOSE("=== parseRefType START ===");
    SourceLocation loc = currentLoc();
    LUC_LOG_TYPE("parseRefType: parsing reference type (&T) at line " << loc.line);
    consume(TokenType::AMPERSAND, DiagCode::E2001, "expected '&'");

    LUC_LOG_TYPE("parseRefType: parsing inner type");
    TypePtr inner = parseBaseType(); // intentionally parseBaseType, not parseType,
                                     // so  &int | string  parses as  (&int) | string
                                     // rather than  &(int | string).
    if (!inner) {
        LUC_LOG_TYPE("parseRefType: ERROR - no inner type after '&'");
        errorAt(DiagCode::E2005, "expected type after '&'");
        return arena_.make<UnknownTypeAST>();
    }

    LUC_LOG_TYPE_VERBOSE("parseRefType: inner type parsed, kind = " << LucDebug::kindToString(inner->kind));
    auto node = arena_.make<RefTypeAST>(std::move(inner));
    node->loc = loc;
    // RefTypeAST itself is not wrapped in wrapNullable — the '?' lives on the
    // inner type or on the enclosing declaration.  A nullable reference is
    // written as  &Vec2?  where '?' attaches to Vec2, not to the ref.
    LUC_LOG_TYPE_VERBOSE("parseRefType: returning RefTypeAST");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parsePtrType
//
// Grammar:  ptr_type := '*' type   (extern / FFI only)
//
// The semantic pass enforces the extern-only restriction.  The parser produces
// PtrTypeAST regardless of context so it can continue and report all errors.
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parsePtrType() {
    LUC_LOG_TYPE_VERBOSE("=== parsePtrType START ===");
    SourceLocation loc = currentLoc();
    LUC_LOG_TYPE("parsePtrType: consuming '*' at line " << loc.line);
    consume(TokenType::MUL, DiagCode::E2001, "expected '*'");

    LUC_LOG_TYPE("parsePtrType: parsing inner type");
    TypePtr inner = parseBaseType(); // same reasoning as parseRefType
    if (!inner) {
        LUC_LOG_TYPE("parsePtrType: inner type parsing FAILED");
        errorAt(DiagCode::E2005, "expected type after '*'");
        return arena_.make<UnknownTypeAST>();
    }

    LUC_LOG_TYPE("parsePtrType: inner type parsed, kind = " << LucDebug::kindToString(inner->kind));
    auto node = arena_.make<PtrTypeAST>(std::move(inner));
    node->loc = loc;
    LUC_LOG_TYPE_VERBOSE("parsePtrType: returning PtrTypeAST");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseFuncType
//
// Grammar:
//   func_type := '(' [ param_types ] ')' [ return_type ]
//              | '(' '(' [ param_types ] ')' [ return_type ] ')' '?'
//
// The outer '(' starts either:
//   a) a normal function type:          (int) string
//   b) a nullable function type:        ((int) string)?
//      — the outer parens wrap a complete function type, followed by '?'.
//
// Disambiguating (a) vs (b):
//   Peek inside the outer '(' — if the very next token is also '(' then we
//   are in form (b) and need to parse the inner function type, consume ')',
//   then consume '?'.
//
// Parameter types in function types discard names — only the type matters.
// The parameter list here is  type { ',' type }  not  name type { ',' name type }.
// However, Luc allows annotated params in type position too (e.g. inside type
// alias  type Callback = (event Event) bool ), so we parse optionally:
//   If IDENTIFIER follows and the next-next token looks like a type, consume
//   the name and then the type; otherwise parse just the type.
//
// Variadic params in function types:  args ...int  →  just the type is stored.
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parseFuncType(bool allowQualifiers) {
    LUC_LOG_TYPE("parseFuncType");
    
    // ── Parse type qualifiers - just collect strings, don't validate ─────────
    std::vector<InternedString> rawQualifiers;
    if (allowQualifiers) {
        while (check(TokenType::TILDE)) {
            advance(); // consume '~'
            
            if (!check(TokenType::IDENTIFIER)) {
                errorAt(DiagCode::E2003, "expected qualifier name after '~'");
                break;
            }
            
            // Just store the name as-is, no validation (semantic phase will validate)
            rawQualifiers.push_back(pool_.intern(advance().value));
            LUC_LOG_TYPE_VERBOSE("\tqualifier: '~" << pool_.lookup(rawQualifiers.back()) << "'");
        }
    }
    
    // ── Check if this is a nullable function: '(' at start? ───────────────────
    bool isNullableFunction = false;
    SourceLocation nullableLoc;
    
    // Check for outer '(' that indicates nullable function
    if (check(TokenType::LPAREN)) {
        size_t savedPos = pos_;
        int parenDepth = 1;
        advance(); // consume the first '('
        
        while (!isAtEnd() && parenDepth > 0) {
            if (check(TokenType::LPAREN)) parenDepth++;
            else if (check(TokenType::RPAREN)) parenDepth--;
            else if (check(TokenType::TILDE)) {
                advance();
                if (check(TokenType::IDENTIFIER)) advance();
                continue;
            }
            advance();
        }
        
        bool hasQuestion = check(TokenType::QUESTION);
        pos_ = savedPos;
        
        if (hasQuestion) {
            isNullableFunction = true;
            nullableLoc = currentLoc();
            advance(); // consume the outer '('
        }
    }
    
    // ── Parse the actual function type (inner part) ───────────────────────────
    consume(TokenType::LPAREN, "expected '(' for function type");
    
    // Build a single parameter group for this function type
    ParamGroup paramGroup;
    
    if (!check(TokenType::RPAREN)) {
        do {
            SourceLocation paramLoc = currentLoc();
            std::string name = "";
            bool variadic = match(TokenType::VARIADIC);

            // Optional parameter name (ignore it in type position)
            if (check(TokenType::IDENTIFIER)) {
                bool nextIsType = false;
                TokenType nextType = peekNext().type;
                switch (nextType) {
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
                    case TokenType::IDENTIFIER:
                    case TokenType::LPAREN:
                    case TokenType::AMPERSAND:
                    case TokenType::MUL:
                    case TokenType::LBRACKET:
                        nextIsType = true;
                        break;
                    default:
                        break;
                }
                
                if (nextIsType) {
                    name = advance().value; // consume parameter name, ignore it mostly
                    LUC_LOG_TYPE_EXTREME("parseFuncType: ignoring parameter name '" << name << "'");
                }
            }
            
            TypePtr paramType = parseType();
            if (!paramType) {
                errorAt(DiagCode::E2005, "expected parameter type");
                break;
            }
            
            auto paramNode = arena_.make<ParamAST>();
            paramNode->loc = paramLoc;
            paramNode->name = std::move(pool_.intern(name));
            paramNode->type = std::move(paramType);
            paramNode->isVariadic = variadic;
            paramGroup.push_back(std::move(paramNode));
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "expected ')' after parameter list");
    
    TypePtr returnType = nullptr;
    if (looksLikeType() && !check(TokenType::LBRACE)) {
        returnType = parseType();
    }
    
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->sig.rawQualifiers = std::move(rawQualifiers);
    funcType->sig.paramGroups.push_back(std::move(paramGroup));  // Single group for function type
    funcType->sig.returnType = std::move(returnType);
    
    // ── Handle nullable RETURN type: (params) ret? ────────────────────────────
    if (match(TokenType::QUESTION)) {
        auto nullableReturn = arena_.make<NullableTypeAST>(std::move(funcType));
        return nullableReturn;
    }
    
    // ── Handle nullable FUNCTION: ( (params) ret )? ───────────────────────────
    if (isNullableFunction) {
        consume(TokenType::RPAREN, "expected ')' to close nullable function wrapper");
        consume(TokenType::QUESTION, "expected '?' for nullable function");
        
        auto nullableFunc = arena_.make<NullableTypeAST>(std::move(funcType));
        nullableFunc->loc = nullableLoc;
        return nullableFunc;
    }
    
    return funcType;
}

// ─────────────────────────────────────────────────────────────────────────────
// wrapNullable
//
// If the current token is '?', consume it and wrap inner in NullableTypeAST.
// Otherwise return inner unchanged.
//
// Called at the end of every concrete type parser that can carry '?':
//   primitive, named, array kinds.
//
// NOT called by:
//   parseRefType / parsePtrType  — '?' in  &Vec2?  attaches to Vec2, not to &.
//   parseFuncType                — nullable function uses the outer-paren form.
//   parseType (union)            — '?' on a union would need outer parens first.
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::wrapNullable(TypePtr inner) {
    if (!match(TokenType::QUESTION)) {
        LUC_LOG_TYPE_VERBOSE("wrapNullable: no '?' token, returning inner type unchanged");
        return inner;
    }

    LUC_LOG_TYPE("wrapNullable: consuming '?' token, wrapping "
                 << LucDebug::kindToString(inner->kind) << " in NullableTypeAST");
    SourceLocation loc = inner->loc; // loc spans from the base type
    auto node = arena_.make<NullableTypeAST>(std::move(inner));
    node->loc = loc;
    LUC_LOG_TYPE_VERBOSE("wrapNullable: returning NullableTypeAST");
    return node;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseGenericArgs
//
// Grammar:  generic_args := '<' type { [','] type } '>'
//
// Used by:
//   parseNamedType()  — Buffer<int>, Map<K, V>
//   parseCallExpr()   — explicit generic instantiation at call sites (ParserExpr.cpp)
//
// Returns an empty vector on failure (error already recorded).
//
// The LESS token ('<') is consumed by the caller (parseNamedType already
// checked it) — this function starts immediately after it.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<TypePtr> Parser::parseGenericArgs() {
    LUC_LOG_TYPE_VERBOSE("=== parseGenericArgs START ===");
    std::vector<TypePtr> args;

    consume(TokenType::LESS, DiagCode::E2001, "expected '<' to open generic arguments");

    if (check(TokenType::GREATER)) {
        // Empty generic arg list — semantically probably an error but we produce
        // the empty vector and let the semantic pass report it.
        LUC_LOG_TYPE("parseGenericArgs: empty generic argument list");
        advance();
        LUC_LOG_TYPE_VERBOSE("=== parseGenericArgs END (empty) ===");
        return args;
    }

    int argCount = 0;
    do {
        // Optional comma between args.
        match(TokenType::COMMA);

        // Trailing comma before '>' is allowed.
        if (check(TokenType::GREATER))
            break;

        LUC_LOG_TYPE_VERBOSE("parseGenericArgs: parsing argument " << argCount + 1);
        TypePtr arg = parseType();
        if (!arg) {
            errorAt(DiagCode::E2005, "expected type inside generic argument list");
            break;
        }
        args.push_back(std::move(arg));
        argCount++;
        LUC_LOG_TYPE_VERBOSE("parseGenericArgs: parsed argument " << argCount);

    } while (!check(TokenType::GREATER) && !isAtEnd());

    consume(TokenType::GREATER, DiagCode::E2001, "expected '>' to close generic argument list");
    LUC_LOG_TYPE("parseGenericArgs: parsed " << argCount << " generic argument(s)");
    LUC_LOG_TYPE_VERBOSE("=== parseGenericArgs END ===");
    return args;
}