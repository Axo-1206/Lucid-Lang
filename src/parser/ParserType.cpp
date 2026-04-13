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
#include "diagnostics/DiagnosticCodes.hpp"

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
//         | type '|' type { '|' type }
//
// The union chain is left-associative but we flatten it into a single
// UnionTypeAST rather than nesting, so  int | string | bool  becomes one node
// with three members — exactly what UnionTypeAST.members is designed for.
//
// Note on '|' ambiguity:
//   In a type position, PIPE is always the union separator.
//   In an expression position, PIPE is bitwise-OR (BinaryExprAST).
//   The parser knows which context it is in by how it arrived here — callers
//   of parseType() are always in a type-annotation position (after a name in
//   a declaration, after ':', as a generic arg, etc.).
// ─────────────────────────────────────────────────────────────────────────────

TypePtr Parser::parseType() {
    TypePtr first = parseBaseType();
    if (!first)
        return nullptr;

    // Check for union continuation: '|' type { '|' type }
    if (!check(TokenType::PIPE)) {
        return first;
    }

    // Build a UnionTypeAST and collect all members.
    auto unionNode = std::make_unique<UnionTypeAST>();
    unionNode->loc = first->loc;
    unionNode->members.push_back(std::move(first));

    while (match(TokenType::PIPE)) {
        TypePtr next = parseBaseType();
        if (!next) {
            errorAt(DiagCode::E2005, "expected type after '|'");
            break;
        }
        unionNode->members.push_back(std::move(next));
    }

    // A union with only one member should never happen (we stop collecting
    // after the first parse fails), but guard defensively.
    if (unionNode->members.size() == 1) {
        // Unwrap — nothing to union with.
        return std::move(unionNode->members[0]);
    }

    return unionNode;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseBaseType  — single non-union type
//
// Dispatches on the current token to the specific sub-parser.  Each sub-parser
// is responsible for consuming the tokens that make up its form and calling
// wrapNullable() on its result before returning.
// ─────────────────────────────────────────────────────────────────────────────

TypePtr Parser::parseBaseType() {
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
            return parsePrimitiveType();

        // ── Named (user-defined) type ─────────────────────────────────────────
        case TokenType::IDENTIFIER:
            return parseNamedType();

        // ── Array types  [N]T  /  []T  /  [*]T ───────────────────────────────
        case TokenType::LBRACKET:
            return parseArrayType();

        // ── Reference  &T ─────────────────────────────────────────────────────
        case TokenType::AMPERSAND:
            return parseRefType();

        // ── Raw pointer  *T  (extern/FFI only) ────────────────────────────────
        case TokenType::MUL:
            return parsePtrType();

        // ── Function type  '(' params ')' [ ret ] ─────────────────────────────
        // A '(' in type position always starts a function type.  A grouped
        // expression would only appear in expression context, never here.
        case TokenType::LPAREN:
            return parseFuncType();

        default:
            // Not a recognisable type start — caller decides if that is an error.
            return nullptr;
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
    SourceLocation loc = currentLoc();
    Token tok = advance();

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
            return nullptr;
    }

    auto node = std::make_unique<PrimitiveTypeAST>(kind);
    node->loc = loc;
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
    SourceLocation loc = currentLoc();
    std::string name = advance().value; // consume IDENTIFIER

    auto node = std::make_unique<NamedTypeAST>(std::move(name));
    node->loc = loc;

    // Optional generic argument list: '<' type { ',' type } '>'
    if (check(TokenType::LESS)) {
        node->genericArgs = parseGenericArgs();
    }

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
    SourceLocation loc = currentLoc();
    consume(TokenType::LBRACKET, "expected '['");

    // ── [*]T  —  dynamic array ────────────────────────────────────────────────
    if (check(TokenType::MUL)) {
        advance(); // consume '*'
        consume(TokenType::RBRACKET, DiagCode::E2001, "expected ']' after '*' in dynamic array type");

        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[*]'");
            return nullptr;
        }

        auto node = std::make_unique<DynamicArrayTypeAST>(std::move(elem));
        node->loc = loc;
        // Dynamic array types are not nullable by themselves — wrapNullable
        // would produce [*]T? which is a nullable dynamic array, a valid form.
        return wrapNullable(std::move(node));
    }

    // ── []T  — slice ──────────────────────────────────────────────────────────
    if (check(TokenType::RBRACKET)) {
        advance(); // consume ']'

        TypePtr elem = parseType();
        if (!elem) {
            errorAt(DiagCode::E2005, "expected element type after '[]'");
            return nullptr;
        }

        auto node = std::make_unique<SliceTypeAST>(std::move(elem));
        node->loc = loc;
        return wrapNullable(std::move(node));
    }

    // ── [N]T  — fixed array ───────────────────────────────────────────────────
    if (check(TokenType::INT_LITERAL)) {
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
            return nullptr;
        }

        auto node = std::make_unique<FixedArrayTypeAST>(size, std::move(elem));
        node->loc = loc;
        return wrapNullable(std::move(node));
    }

    // ── Unrecognised content between '[' and the next token ───────────────────
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
    return nullptr;
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
    SourceLocation loc = currentLoc();
    consume(TokenType::AMPERSAND, DiagCode::E2001, "expected '&'");

    TypePtr inner = parseBaseType(); // intentionally parseBaseType, not parseType,
                                     // so  &int | string  parses as  (&int) | string
                                     // rather than  &(int | string).
    if (!inner) {
        errorAt(DiagCode::E2005, "expected type after '&'");
        return nullptr;
    }

    auto node = std::make_unique<RefTypeAST>(std::move(inner));
    node->loc = loc;
    // RefTypeAST itself is not wrapped in wrapNullable — the '?' lives on the
    // inner type or on the enclosing declaration.  A nullable reference is
    // written as  &Vec2?  where '?' attaches to Vec2, not to the ref.
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
    SourceLocation loc = currentLoc();
    consume(TokenType::MUL, DiagCode::E2001, "expected '*'");

    TypePtr inner = parseBaseType(); // same reasoning as parseRefType
    if (!inner) {
        errorAt(DiagCode::E2005, "expected type after '*'");
        return nullptr;
    }

    auto node = std::make_unique<PtrTypeAST>(std::move(inner));
    node->loc = loc;
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

TypePtr Parser::parseFuncType() {
    SourceLocation loc = currentLoc();

    // ── Nullable function form:  ((params) ret)? ──────────────────────────────
    // Outer '(' followed immediately by another '(' means the whole function
    // type is wrapped for nullability.
    if (check(TokenType::LPAREN) && peekNext().type == TokenType::LPAREN) {
        advance(); // consume outer '('

        // Parse the inner function type — it starts with '('.
        TypePtr innerFunc = parseFuncType();
        if (!innerFunc) {
            errorAt(DiagCode::E2005, "expected function type inside '(( ))'");
            return nullptr;
        }

        consume(TokenType::RPAREN, DiagCode::E2001, "expected ')' to close nullable function type");
        consume(TokenType::QUESTION, DiagCode::E2001, "expected '?' after ')' in nullable function type");

        // Mark the inner FuncTypeAST as nullable.
        // Down-cast is safe: parseFuncType() always returns FuncTypeAST.
        auto *ft = static_cast<FuncTypeAST *>(innerFunc.get());
        ft->isNullable = true;
        return innerFunc;
    }

    // ── Normal function type:  (params) [ return_type ] ──────────────────────
    consume(TokenType::LPAREN, DiagCode::E2001, "expected '(' at start of function type");

    auto node = std::make_unique<FuncTypeAST>(/*nullable=*/false);
    node->loc = loc;

    // Parse parameter types — stop at ')'.
    if (!check(TokenType::RPAREN)) {
        do {
            // Skip optional comma separator.
            match(TokenType::COMMA);

            // Check for end after consuming comma (trailing comma allowed).
            if (check(TokenType::RPAREN))
                break;

            // Variadic prefix:  '...' before the type (no name in type position)
            bool isVariadic = match(TokenType::VARIADIC);
            (void)isVariadic; // Stored in the type — variadic in func types is
                              // noted but the type itself is still the element type.

            // Optional parameter name — present in type aliases like:
            //   type Callback = (event Event) bool
            // If IDENTIFIER is followed by something that looks like a type start,
            // consume the name and then parse the type.
            if (check(TokenType::IDENTIFIER)) {
                // peek() is IDENTIFIER — check if peekNext looks like a type start.
                // We do this by temporarily advancing and checking.
                TokenType nextTT = peekNext().type;
                bool nextIsType = (nextTT == TokenType::TYPE_BOOL ||
                                   nextTT == TokenType::TYPE_BYTE ||
                                   nextTT == TokenType::TYPE_SHORT ||
                                   nextTT == TokenType::TYPE_INT ||
                                   nextTT == TokenType::TYPE_LONG ||
                                   nextTT == TokenType::TYPE_UBYTE ||
                                   nextTT == TokenType::TYPE_USHORT ||
                                   nextTT == TokenType::TYPE_UINT ||
                                   nextTT == TokenType::TYPE_ULONG ||
                                   nextTT == TokenType::TYPE_INT8 ||
                                   nextTT == TokenType::TYPE_INT16 ||
                                   nextTT == TokenType::TYPE_INT32 ||
                                   nextTT == TokenType::TYPE_INT64 ||
                                   nextTT == TokenType::TYPE_UINT8 ||
                                   nextTT == TokenType::TYPE_UINT16 ||
                                   nextTT == TokenType::TYPE_UINT32 ||
                                   nextTT == TokenType::TYPE_UINT64 ||
                                   nextTT == TokenType::TYPE_FLOAT ||
                                   nextTT == TokenType::TYPE_DOUBLE ||
                                   nextTT == TokenType::TYPE_DECIMAL ||
                                   nextTT == TokenType::TYPE_STRING ||
                                   nextTT == TokenType::TYPE_CHAR ||
                                   nextTT == TokenType::TYPE_ANY ||
                                   nextTT == TokenType::IDENTIFIER ||
                                   nextTT == TokenType::LBRACKET ||
                                   nextTT == TokenType::AMPERSAND ||
                                   nextTT == TokenType::AT ||
                                   nextTT == TokenType::LPAREN ||
                                   nextTT == TokenType::VARIADIC);
                if (nextIsType) {
                    advance(); // consume the parameter name — we discard it
                }
            }

            TypePtr paramType = parseType();
            if (!paramType) {
                errorAt(DiagCode::E2005, "expected parameter type in function type");
                break;
            }
            node->params.push_back(std::move(paramType));

        } while (!check(TokenType::RPAREN) && !isAtEnd());
    }

    consume(TokenType::RPAREN, DiagCode::E2001, "expected ')' to close function type parameter list");

    // Optional return type — any type that is not '|' (which would belong to a
    // union wrapping this entire function type at a higher level).
    // Return type is present if the current token looks like the start of a type.
    if (looksLikeType() && !check(TokenType::PIPE)) {
        node->returnType = parseType();
    }

    return node;
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
    if (!match(TokenType::QUESTION))
        return inner;

    SourceLocation loc = inner->loc; // loc spans from the base type
    auto node = std::make_unique<NullableTypeAST>(std::move(inner));
    node->loc = loc;
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
    std::vector<TypePtr> args;

    consume(TokenType::LESS, DiagCode::E2001, "expected '<' to open generic arguments");

    if (check(TokenType::GREATER)) {
        // Empty generic arg list — semantically probably an error but we produce
        // the empty vector and let the semantic pass report it.
        advance();
        return args;
    }

    do {
        // Optional comma between args.
        match(TokenType::COMMA);

        // Trailing comma before '>' is allowed.
        if (check(TokenType::GREATER))
            break;

        TypePtr arg = parseType();
        if (!arg) {
            errorAt(DiagCode::E2005, "expected type inside generic argument list");
            break;
        }
        args.push_back(std::move(arg));

    } while (!check(TokenType::GREATER) && !isAtEnd());

    consume(TokenType::GREATER, DiagCode::E2001, "expected '>' to close generic argument list");

    return args;
}