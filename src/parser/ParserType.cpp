/**
 * @file ParserType.cpp
 *
 * @responsibility Parses all type annotations and signatures.
 *
 * This file implements the complete type parsing subsystem:
 *   - Root type parser (parseType) with optional nullable suffix
 *   - Base type dispatcher (parseBaseType) for all type forms
 *   - Primitive types (bool, int, float, string, etc.)
 *   - Named types (user-defined types, with optional generic arguments)
 *   - Array types: fixed [N]T, slice []T, dynamic [*]T
 *   - Reference types &T (safe managed references)
 *   - Raw pointer types *T (extern/FFI only)
 *   - Function types with qualifiers, parameter groups, and return lists
 *   - Generic argument lists for type instantiation
 *   - Nullable wrapper (?) post-processing
 *
 * All type parsers consume tokens from the parser's stream and build
 * corresponding TypeAST nodes. They include error recovery and progress
 * guarantees to prevent infinite loops.
 *
 * @related
 *   - Parser.hpp – class declaration and shared utilities
 *   - Parser.cpp – core token stream primitives
 *   - ParserDecl.cpp – declaration parsing (uses type parsers for annotations)
 *   - ParserExpr.cpp – expression parsing (uses type parsers for casts and is-expr)
 *   - ParserStmt.cpp – statement parsing (uses type parsers for for-loop types)
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NAVIGATION – Functions in this file (in order of appearance)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 * ██ Root Type Parsers
 *   parseType()                      – type := base_type [ '?' ]
 *   parseBaseType()                  – dispatches to concrete type parsers
 *
 * ██ Concrete Type Parsers
 *   parsePrimitiveType()             – bool, int, float, string, any, etc.
 *   parseNamedType()                 – IDENTIFIER [ '<' generic_args '>' ]
 *   parseArrayType()                 – [N]T | []T | [*]T
 *   parseRefType()                   – &T (safe reference)
 *   parsePtrType()                   – *T (raw pointer, extern only)
 *   parseFuncType()                  – [qualifiers] param_group+ [ '->' return_list ]
 *
 * ██ Helpers
 *   wrapNullable()                   – optionally wraps type in NullableTypeAST
 *   parseGenericArgs()               – '<' type { ',' type } '>'
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * TYPE GRAMMAR (from LUC_GRAMMAR.md)
 *
 *   type            := base_type [ '?' ]
 *   base_type       := primitive_type | IDENTIFIER | array_type
 *                    | ref_type | ptr_type | func_type
 *   primitive_type  := 'bool' | 'byte' | 'short' | 'int' | 'long'
 *                    | 'ubyte' | 'ushort' | 'uint' | 'ulong'
 *                    | 'int8' | 'int16' | 'int32' | 'int64'
 *                    | 'uint8' | 'uint16' | 'uint32' | 'uint64'
 *                    | 'float' | 'double' | 'decimal'
 *                    | 'string' | 'char' | 'any'
 *   array_type      := '[' INT_LITERAL ']' type   (fixed)
 *                    | '[' ']' type               (slice)
 *                    | '[' '*' ']' type           (dynamic)
 *   ref_type        := '&' type
 *   ptr_type        := '*' type
 *   func_type       := [ qualifier_list ] param_group { param_group }
 *                      [ '->' return_list ]
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * NULLABLE RULES
 *
 *   - '?' suffix attaches to the immediately preceding type.
 *   - Generic arguments always come before '?': List<int>? not List<int?>.
 *   - Not called on ref_type or ptr_type (nullable reference is &T? where '?'
 *     attaches to T, not to &).
 *   - Function types are not nullable directly; use a named type alias.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * LOOP SAFETY & PROGRESS GUARDS
 *
 *   - parseArrayType() includes bracket depth tracking and error recovery.
 *   - parseGenericArgs() uses progress guards to prevent infinite loops.
 *   - All type parsers consume at least one token on success or report an error.
 *
 * ─────────────────────────────────────────────────────────────────────────────
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
// Root Type Parsers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parseType
//
// Root entry point for parsing a type annotation.
//
// Grammar:
//   type := base_type [ '?' ]
//
// Examples:
//   int
//   Vec2?
//   []string
//   &T
//   (x int) -> string
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Calls parseBaseType() to parse the core type (primitive, named, array,
//   reference, pointer, or function type).
// - If the next token is '?', consumes it and wraps the result in a
//   NullableTypeAST.
// - Does NOT consume any tokens beyond the optional '?'.
//
// ─── Return Value ───────────────────────────────────────────────────────────
// - Returns a TypePtr (never nullptr; on error returns UnknownTypeAST).
// - The caller should check the diagnostic engine for errors.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If parseBaseType() fails (returns UnknownTypeAST), returns that node
//   without attempting to parse '?' (since the base type is already invalid).
// - All errors are reported by the sub‑parsers.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - This function is called from many contexts: variable declarations,
//   function parameters, struct fields, return types, generic arguments,
//   type casts, and is‑expressions.
// - The '?' suffix is only valid on value types (primitives, structs, arrays,
//   named aliases). The semantic pass enforces this restriction.
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
// parseBaseType
//
// Parses a single type (without the optional nullable suffix).
//
// Grammar:
//   base_type := primitive_type
//              | named_type
//              | array_type
//              | ref_type
//              | ptr_type
//              | func_type
//
// ─── Dispatch Logic ─────────────────────────────────────────────────────────
// Dispatches based on the current token:
//   - Primitive type keywords → parsePrimitiveType()
//   - IDENTIFIER              → parseNamedType()
//   - '['                     → parseArrayType()
//   - '&'                     → parseRefType() (then wrapNullable)
//   - '*'                     → parsePtrType() (then wrapNullable)
//   - '(' or '~'              → parseFuncType()
//   - default                 → returns UnknownTypeAST
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the tokens that form the base type (delegates to sub‑parsers).
// - Does NOT consume the optional '?' suffix – that is handled by parseType()
//   calling wrapNullable() after parseBaseType() returns.
// - For '&' and '*', the sub‑parser returns the raw type; parseBaseType then
//   calls wrapNullable() to handle any trailing '?'.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If the current token does not match any recognised type start, returns
//   UnknownTypeAST (no error reported; the caller decides if this is an error).
// - Sub‑parsers report their own errors.
//
// ─── Notes ──────────────────────────────────────────────────────────────────
// - This function intentionally does NOT handle '?' – that is deferred to
//   parseType() or wrapNullable() so that all type forms can be uniformly
//   made nullable.
// - The dispatch order matches the precedence in the grammar (primitive
//   keywords before identifiers, etc.).
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
            return parseFuncType();
        case TokenType::TILDE:
            LUC_LOG_TYPE_VERBOSE("parseBaseType: dispatching to parseFuncType (qualifier start)");
            return parseFuncType();

        default:
            // Not a recognisable type start — caller decides if that is an error.
            LUC_LOG_TYPE("parseBaseType: unrecognized type start: '" << peek().value << "'");
            return arena_.make<UnknownTypeAST>();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Concrete Type Parsers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// parsePrimitiveType
//
// Parses a primitive type keyword.
//
// Grammar:
//   primitive_type := 'bool' | 'byte' | 'short' | 'int' | 'long'
//                   | 'ubyte' | 'ushort' | 'uint' | 'ulong'
//                   | 'int8' | 'int16' | 'int32' | 'int64'
//                   | 'uint8' | 'uint16' | 'uint32' | 'uint64'
//                   | 'float' | 'double' | 'decimal'
//                   | 'string' | 'char' | 'any'
//
// Examples:
//   int           → PrimitiveTypeAST { primitiveKind = Int }
//   string        → PrimitiveTypeAST { primitiveKind = String }
//   float32?      → (after wrapNullable) NullableTypeAST { inner = Float32 }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the primitive type keyword token.
// - Does NOT consume any tokens beyond the keyword.
//
// ─── Kind Mapping ───────────────────────────────────────────────────────────
// Maps each TokenType to the corresponding PrimitiveKind enum value.
// The mapping is exhaustive – every primitive keyword has an entry.
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - If called on a non‑primitive token (should not happen), reports an internal
//   error and returns UnknownTypeAST.
// - The caller (parseBaseType) ensures this function is only called on valid
//   primitive tokens.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   PrimitiveTypeAST {
//       primitiveKind: PrimitiveKind (Bool, Int, String, Float, etc.)
//   }
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
// Parses a named type (user-defined type reference), optionally with generic
// arguments.
//
// Grammar:
//   named_type := IDENTIFIER [ '<' type { [','] type } '>' ] [ '?' ]
//
// Examples:
//   Vec2
//   Buffer<int>
//   Map<string, Vec2>
//   Option<T>?        (nullable after generic args)
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes an IDENTIFIER (type name).
// - If the next token is '<', calls parseGenericArgs() to consume the generic
//   argument list.
// - Does NOT consume the optional '?' suffix – that is handled by the caller
//   (parseBaseType calls wrapNullable on the result).
//
// ─── Generic Arguments ──────────────────────────────────────────────────────
// - Generic arguments are parsed as a comma‑separated list of types inside
//   angle brackets: < T, U, V >
// - The argument list may be empty (e.g., `Buffer<>` – allowed by grammar).
// - The semantic pass resolves the type name and validates the argument count.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing type name (no IDENTIFIER): error reported by caller before call.
// - Generic argument parsing errors are reported by parseGenericArgs().
// - Returns a NamedTypeAST even if generic args failed to parse (empty list).
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   NamedTypeAST {
//       name:         InternedString (the type name)
//       genericArgs:  vector<TypePtr> (empty if no generic args)
//   }
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
// Parses an array type – fixed, slice, or dynamic.
//
// Grammar:
//   array_type := '[' INT_LITERAL ']' type   -- fixed:   [100]int
//               | '[' ']' type               -- slice:   []int
//               | '[' '*' ']' type           -- dynamic: [*]int
//
// Examples:
//   [4]float     → FixedArrayTypeAST { size=4, element=Float }
//   []int        → SliceTypeAST { element=Int }
//   [*]Vec2      → DynamicArrayTypeAST { element=Vec2 }
//   [][*]float   → SliceTypeAST { element=DynamicArrayTypeAST{Float} }
//   [4][4]float  → FixedArrayTypeAST { size=4, element=FixedArrayTypeAST{4,Float} }
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the opening '['.
// - Determines the array kind based on the token after '[':
//     * '*'        → dynamic array: consumes '*', expects ']', then element type
//     * ']'        → slice: consumes ']', then element type
//     * INT_LITERAL → fixed array: consumes the integer, expects ']', then element type
// - After parsing the element type, calls wrapNullable() to handle optional '?'.
// - Consumes all tokens that are part of the array type (including nested arrays).
//
// ─── Multidimensional Arrays ────────────────────────────────────────────────
// - Handled naturally because the element type is parsed via parseType(), which
//   will recursively call parseArrayType() if the element itself starts with '['.
// - Example: [4][4]float → first parses [4], then element type is [4]float.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing closing ']' after '*' or size: reports error.
// - Invalid size literal (non-integer or overflow): reports error, size set to 0.
// - Missing element type after array brackets: reports error, returns UnknownTypeAST.
// - Unrecognised content inside brackets: reports error, skips to matching ']'.
//
// ─── Loop Safety ────────────────────────────────────────────────────────────
// - Brackets are balanced using a depth counter when skipping to the matching ']'
//   on error recovery, guaranteeing progress.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   FixedArrayTypeAST   → { size: uint64_t, element: TypePtr }
//   SliceTypeAST        → { element: TypePtr }
//   DynamicArrayTypeAST → { element: TypePtr }
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
// Parses a safe managed reference type: &T
//
// Grammar:
//   ref_type := '&' type
//
// Example:
//   &int           → RefTypeAST { inner = Int }
//   &Vec2          → RefTypeAST { inner = Vec2 }
//   &Vec2?         → RefTypeAST { inner = NullableTypeAST { Vec2 } }
//
// ─── Semantics ──────────────────────────────────────────────────────────────
// - References are always valid (non‑nullable by default).
// - To express a nullable reference, write &T? where '?' attaches to T.
// - References are used for shared ownership without copying.
// - Field access through references is supported (ref.field).
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the '&' token.
// - Parses the inner type using parseBaseType() (not parseType()).
//   This ensures that &int | string parses as (&int) | string rather than
//   &(int | string) – note that union types are not in the grammar, but this
//   avoids ambiguity if they were added.
// - Does NOT call wrapNullable() on the result – the '?' suffix attaches to the
//   inner type, not to the reference itself.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing inner type after '&': reports error, returns UnknownTypeAST.
// - Inner type parsing errors are reported by parseBaseType().
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   RefTypeAST {
//       inner: TypePtr (the referenced type)
//   }
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
// Parses a raw pointer type: *T (only valid on @extern declarations).
//
// Grammar:
//   ptr_type := '*' type
//
// Example:
//   *uint8         → PtrTypeAST { inner = Uint8 }
//   *VkInstance    → PtrTypeAST { inner = NamedTypeAST("VkInstance") }
//
// ─── The Sealed Conduit Model ───────────────────────────────────────────────
// - Raw pointers are "sealed conduits" – they cannot be dereferenced directly.
// - Allowed operations: store, pass to @extern, nil check, pointer intrinsics.
// - Forbidden operations: dereference (*ptr), field access (ptr.f), indexing,
//   arithmetic (use #ptrOffset instead).
// - Boundary crossing: #ptrToRef(ptr) -> &T, #refToPtr(ref) -> *T.
//
// ─── Restrictions (Enforced by Semantic Pass) ───────────────────────────────
// - Raw pointers are only valid inside @extern‑decorated declarations.
// - The parser produces PtrTypeAST regardless of context; the semantic pass
//   reports an error if a raw pointer appears outside an extern declaration.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes the '*' token.
// - Parses the inner type using parseBaseType() (same reasoning as parseRefType).
// - Does NOT call wrapNullable() on the result – the '?' suffix attaches to the
//   inner type, not to the pointer itself.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing inner type after '*': reports error, returns UnknownTypeAST.
// - Inner type parsing errors are reported by parseBaseType().
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   PtrTypeAST {
//       inner: TypePtr (the pointed‑to type)
//   }
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
// Parses a function type annotation.
//
// Grammar:
//   func_type := [ qualifier_list ] param_group { param_group } [ '->' return_list ]
//   qualifier_list := { '~' IDENTIFIER }
//   param_group     := '(' [ param_list ] ')'
//   return_list     := '(' [ return_type { ',' return_type } ] ')' | return_type
//
// Examples:
//   (x int) -> int
//   ~async (url string) -> string
//   (a int)(b int) -> int                    (curried)
//   (src string) -> (int, string)            (multiple returns)
//   () -> int                                (zero parameters)
//
// ─── Qualifier Handling ─────────────────────────────────────────────────────
// - Qualifiers (e.g., ~async, ~nullable, ~parallel) are stored raw in
//   rawQualifiers as InternedStrings.
// - The parser does NOT validate qualifier names – that is deferred to the
//   semantic phase which resolves them via QualifierRegistry.
// - The qualifiers bitmask (sig.qualifiers) is initialised to 0 and filled by
//   the semantic phase.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - Consumes zero or more '~' IDENTIFIER pairs (qualifiers).
// - Consumes one or more parameter groups via parseParamGroup().
// - If '->' is present, consumes it and parses the return list via parseReturnList().
// - Does NOT consume any tokens beyond the return list.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - If no parameter group is found after qualifiers, reports an error and returns
//   UnknownTypeAST (function type must have at least one parameter group).
// - Missing parameter name inside a group is handled by parseParamGroup().
// - Return list errors are reported by parseReturnList().
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   FuncTypeAST {
//       sig: FuncSignature {
//           rawQualifiers: vector<InternedString>
//           qualifiers:    uint32_t (0 initially, set by semantic pass)
//           paramGroups:   vector<vector<ParamPtr>>
//           returnTypes:   vector<TypePtr>
//       }
//   }
// ─────────────────────────────────────────────────────────────────────────────
TypePtr Parser::parseFuncType() {
    LUC_LOG_TYPE("parseFuncType");

    std::vector<InternedString> rawQualifiers;

    // ── Collect raw qualifier names — resolved to bitmask by semantic phase ──
    // The parser does NOT validate qualifier names here. Raw strings are stored
    // in rawQualifiers; the semantic phase resolves them via QualifierRegistry.
    while (check(TokenType::TILDE)) {
        advance(); // consume '~'
        if (!check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        std::string qualName = advance().value;
        rawQualifiers.push_back(pool_.intern(qualName));
        LUC_LOG_TYPE_VERBOSE("parseFuncType: raw qualifier stored '~" << qualName << "'");
    }

    // ── Parse one or more parameter groups ──────────────────────────────────
    std::vector<ParamGroup> paramGroups;
    while (check(TokenType::LPAREN)) {
        paramGroups.push_back(parseParamGroup());
    }

    // After collecting qualifiers, ensure at least one parameter group is present. 
    // If not, report an error and return an UnknownTypeAST
    if (paramGroups.empty()) {
        errorAt(DiagCode::E2001, "function type must have at least one parameter group '(' ... ')'");
        return arena_.make<UnknownTypeAST>();
    }

    // ── Parse return list after '->' (if present) ───────────────────────────
    std::vector<TypePtr> returnTypes;

    if (match(TokenType::ARROW)) {
        returnTypes = parseReturnList();
    }

    // ── Build the function type node ─────────────────────────────────────────
    // sig.qualifiers starts at 0 — filled by the semantic phase.
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->sig.qualifiers = 0;
    funcType->sig.rawQualifiers = std::move(rawQualifiers);
    funcType->sig.paramGroups = std::move(paramGroups);
    funcType->sig.returnTypes = std::move(returnTypes);

    return funcType;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// wrapNullable
//
// Optionally wraps a type in a NullableTypeAST if the next token is '?'.
//
// Grammar:
//   nullable_type := type '?'
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - If the current token is '?', consumes it and returns a NullableTypeAST
//   wrapping the inner type.
// - Otherwise, returns the inner type unchanged.
// - Does NOT consume any tokens beyond the optional '?'.
//
// ─── Where It Is Called ─────────────────────────────────────────────────────
// Called by parseType() and by specific type parsers that can be followed by '?':
//   - parsePrimitiveType()   – int?, string?, bool?
//   - parseNamedType()       – Vec2?, Buffer<int>?
//   - parseArrayType()       – [4]float?, []int?, [*]T?
//
// ─── Where It Is NOT Called ─────────────────────────────────────────────────
//   - parseRefType()   – &T? has '?' attach to T, not to & (handled by wrapNullable
//                        on the inner type after parseRefType returns)
//   - parsePtrType()   – same reasoning as ref type
//   - parseFuncType()  – function types are not directly nullable; use a named
//                        type alias instead
//
// ─── Error Handling ─────────────────────────────────────────────────────────
// - No error reporting – if '?' is present, it is consumed unconditionally.
// - The semantic pass enforces that '?' is only valid on value types.
//
// ─── Resulting AST ──────────────────────────────────────────────────────────
//   NullableTypeAST {
//       inner: TypePtr (the type being made nullable)
//   }
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
// Parses a generic argument list: < type { ',' type } >
//
// Grammar:
//   generic_args := '<' type { [','] type } '>'
//
// Examples:
//   <int>
//   <string, Vec2>
//   <T, U, V>
//   <>                    (empty – allowed by grammar)
//
// ─── Preconditions ──────────────────────────────────────────────────────────
// - The caller (parseNamedType or parsePostfixExpr) has already consumed the
//   opening '<' token.
// - This function starts immediately after the '<'.
//
// ─── Token Consumption ───────────────────────────────────────────────────────
// - If the next token is '>', consumes it and returns an empty vector (empty list).
// - Otherwise, repeatedly parses types:
//     * Optional comma before each type (allows trailing commas)
//     * Parses a type via parseType()
//     * Adds the type to the argument list
// - Stops when '>' is encountered or EOF is reached.
// - Consumes the closing '>'.
//
// ─── Loop Safety & Progress Guarantee ───────────────────────────────────────
// - Uses a progress guard: saves pos_ before each parseType() call.
// - If parseType() makes no progress (pos_ == savedPos):
//     * Reports an error
//     * Consumes one token to avoid infinite loop
//     * Breaks out of the loop
// - The loop terminates when '>' is found or after a fatal error.
//
// ─── Error Handling & Recovery ──────────────────────────────────────────────
// - Missing '>' at the end: consume() reports error and recovers.
// - Missing type after comma: parseType() reports error, loop may continue
//   if a comma or '>' is found.
// - Returns an empty vector on error (error already reported).
//
// ─── Usage Contexts ─────────────────────────────────────────────────────────
// - parseNamedType()   – for generic type instantiation: Buffer<int>
// - parseCallExpr()    – for explicit generic function calls: process<int>(42)
//   (via parsePostfixExpr in ParserExpr.cpp)
//
// ─── Result ─────────────────────────────────────────────────────────────────
//   std::vector<TypePtr> – the parsed generic arguments in order.
//   Returns an empty vector on error or for an empty list <>.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<TypePtr> Parser::parseGenericArgs() {
    LUC_LOG_TYPE_VERBOSE("=== parseGenericArgs START ===");
    std::vector<TypePtr> args;

    consume(TokenType::LESS, DiagCode::E2001, "expected '<' to open generic arguments");

    if (check(TokenType::GREATER)) {
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
        
        // Progress tracking
        std::size_t savedPos = pos_;
        TypePtr arg = parseType();
        if (pos_ == savedPos) {
            errorAt(DiagCode::E2005, "expected type inside generic argument list");
            // Consume the offending token to avoid infinite loop
            if (!isAtEnd()) advance();
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