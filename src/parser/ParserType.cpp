/**
 * @file ParserType.cpp
 * @brief Parses all type annotations and signatures.
 * 
 * ============================================================================
 * FILE OVERVIEW
 * ============================================================================
 * 
 * This file implements the type parsing subsystem for the Luc language.
 * 
 * ## Type Categories
 * 
 *   - Primitive types    : `int`, `float`, `string`, `bool`, `any`, etc.
 *   - Named types        : `Vec2`, `Buffer<int>`, `Option<T>`
 *   - Array types        : `[N]T` (fixed), `[]T` (slice), `[*]T` (dynamic)
 *   - Reference types    : `&T` (safe managed reference)
 *   - Pointer types      : `*T` (raw pointer, sealed conduit)
 *   - Function types     : `(a int) -> string`, `~async (url string) -> string`
 *   - Nullable types     : `int?`, `Vec2?` (postfix '?' suffix)
 * 
 * ## Type Grammar (simplified)
 * 
 *   type        := base_type [ '?' ]
 *   base_type   := primitive_type | named_type | array_type | ref_type
 *                | ptr_type | func_type
 * 
 * ## Nullable Rules
 * 
 *   - '?' attaches to the immediately preceding type
 *   - Generic arguments always come before '?': `List<int>?` not `List<int?>`
 *   - Function types cannot be nullable directly (use a type alias)
 * 
 * ## Dispatch Order (parseBaseType)
 * 
 *   1. Primitive keywords → parsePrimitiveType()
 *   2. IDENTIFIER        → parseNamedType()
 *   3. '['              → parseArrayType()
 *   4. '&'              → parseRefType()
 *   5. '*'              → parsePtrType()
 *   6. '(' or '~'       → parseFuncType()
 *   7. default          → error + UnknownTypeAST
 * 
 * ## Loop Safety
 * 
 *   - parseArrayType() includes bracket depth tracking for error recovery
 *   - parseGenericArgs() uses saved position pattern
 * 
 * @see ParserDecl.cpp for type usage in declarations
 * @see ParserExpr.cpp for type casts and `is` expressions
 * @see LUC_GRAMMAR.md for type grammar rules
 */

#include "Parser.hpp"
#include "ast/support/InternedString.hpp"
#include "diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <string>

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

// ============================================================================
// Primitive Type
// ============================================================================
// 
// parsePrimitiveType() parses a primitive type keyword.
// 
// Grammar: `bool` | `int` | `float` | `string` | `any` | ...
// 
// The complete list includes:
//   - Boolean:   bool
//   - Signed:    byte, short, int, long, int8, int16, int32, int64
//   - Unsigned:  ubyte, ushort, uint, ulong, uint8, uint16, uint32, uint64
//   - Floating:  float, double, decimal
//   - Text:      string, char
//   - Dynamic:   any
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at a primitive keyword
// On exit:  positioned after the keyword
// 
// ─── Kind Mapping ─────────────────────────────────────────────────────────
// Maps each TokenType to the corresponding PrimitiveKind enum value.
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Called only on valid primitive tokens (caller guarantees)
// - Internal error: returns UnknownTypeAST if called on non-primitive
// ============================================================================

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

// ============================================================================
// Named Type
// ============================================================================
// 
// parseNamedType() parses a user-defined type reference with optional generic
// arguments.
// 
// Grammar: IDENTIFIER [ '<' type { ',' type } '>' ]
// 
// Examples:
//   Vec2
//   Buffer<int>
//   Map<string, Vec2>
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at IDENTIFIER (type name)
// On exit:  positioned after generic arguments (or after name if none)
// 
// ─── Generic Arguments ────────────────────────────────────────────────────
//   - Parsed via parseGenericArgs() (consumes '<' ... '>')
//   - Empty list `<` `>` is allowed
//   - Semantic pass validates argument count against declaration
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
// - Missing type name: returns UnknownTypeAST
// - Generic argument errors: reported by parseGenericArgs()
// ============================================================================

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

// ============================================================================
// Reference Type
// ============================================================================
// 
// parseRefType() parses a safe managed reference type.
// 
// Grammar: '&' type
// 
// Example: `&int`, `&Vec2`
// 
// ─── Semantics ────────────────────────────────────────────────────────────
//   - References are always valid (non‑nullable by default)
//   - To express a nullable reference: `&T?` where '?' attaches to T
//   - Used for shared ownership without copying
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '&'
// On exit:  positioned after the inner type
// 
// ─── Note ─────────────────────────────────────────────────────────────────
//   - The inner type is parsed via parseBaseType() (not parseType()) to avoid
//     consuming a trailing '?' that belongs to the inner type
//   - RefTypeAST itself is not wrapped in nullable – '?' lives on inner type
// ============================================================================

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

// ============================================================================
// Pointer Type (Sealed Conduit)
// ============================================================================
// 
// parsePtrType() parses a raw pointer type.
// 
// Grammar: '*' type
// 
// Example: `*uint8`, `*VkInstance`
// 
// ─── The Sealed Conduit Model ─────────────────────────────────────────────
//   - Raw pointers are "sealed conduits" – cannot be dereferenced directly
//   - Allowed: store, pass to @extern, nil check, pointer intrinsics
//   - Forbidden: dereference (*ptr), field access, indexing, arithmetic
//   - Boundary crossing: #ptrToRef(ptr) → &T, #refToPtr(ref) → *T
// 
// ─── Restrictions (Semantic Pass) ─────────────────────────────────────────
//   - Raw pointers only valid inside @extern‑decorated declarations
//   - Parser produces PtrTypeAST regardless; semantic pass reports error
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at '*'
// On exit:  positioned after the inner type
// 
// ─── Note ─────────────────────────────────────────────────────────────────
//   - Same note as RefType: inner type parsed via parseBaseType()
//   - '?' attaches to inner type, not to the pointer itself
// ============================================================================

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

// ============================================================================
// Function Type
// ============================================================================
// 
// parseFuncType() parses a function type annotation.
// 
// Grammar: [ qualifier_list ] param_group { param_group } [ '->' return_list ]
// 
// Examples:
//   (x int) -> int
//   ~async (url string) -> string
//   (a int)(b int) -> int                    (curried)
//   (src string) -> (int, string)            (multiple returns)
//   () -> int                                (zero parameters)
// 
// ─── Qualifier Handling ───────────────────────────────────────────────────
//   - Qualifiers stored raw in rawQualifiers (InternedString)
//   - Validation and bitmask computation deferred to semantic phase
//   - `~parallel` does NOT affect type equality; `~async`/`~nullable` do
// 
// ─── Parameter Groups (Flat Representation) ───────────────────────────────
//   - Parameters are flattened into `allParams` vector
//   - `groupSizes` records how many parameters belong to each curry group
//   - Example: `(a int)(b int)(c int)` → allParams=[a,b,c], groupSizes=[1,1,1]
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
// On entry: positioned at first '(' or '~' (qualifier)
// On exit:  positioned after return list (or after last param group if void)
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
//   - Parameter group loop continues while '(' is found
//   - parseParamGroup() guarantees progress
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing '(' after qualifiers: reports error, returns UnknownTypeAST
//   - Empty parameter list `()` is allowed (zero parameters)
//   - Return list errors reported by parseReturnList()
// ============================================================================

TypePtr Parser::parseFuncType() {
    SourceLocation loc = ts_.currentLoc();
    auto funcType = arena_.make<FuncTypeAST>();
    funcType->loc = loc;

    // Parse raw qualifiers and build bitmask
    std::vector<InternedString> rawQuals;
    uint32_t qualMask = 0;
    while (ts_.check(TokenType::TILDE)) {
        ts_.advance();
        if (!ts_.check(TokenType::IDENTIFIER)) {
            errorAt(DiagCode::E2003, "expected qualifier name after '~'");
            break;
        }
        InternedString q = pool_.intern(ts_.advance().value);
        rawQuals.push_back(q);
        std::string_view qstr = pool_.lookup(q);
        if (qstr == "async") qualMask |= QualifierBits::Async;
        else if (qstr == "nullable") qualMask |= QualifierBits::Nullable;
        else if (qstr == "parallel") qualMask |= QualifierBits::Parallel;
        // Other qualifiers are ignored here; semantic pass will report errors
    }
    auto qBuilder = arena_.makeBuilder<InternedString>();
    for (auto& q : rawQuals) qBuilder.push_back(std::move(q));
    funcType->rawQualifiers = qBuilder.build();
    funcType->qualifiers = qualMask;

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

// ============================================================================
// Generic Arguments
// ============================================================================
// 
// parseGenericArgs() parses a generic argument list for type instantiation.
// 
// Grammar: '<' type { ',' type } '>'
// 
// Examples:
//   <int>
//   <string, Vec2>
//   <T, U, V>
//   <>                    (empty – allowed)
// 
// ─── Preconditions ────────────────────────────────────────────────────────
//   - The caller (parseNamedType or parsePostfixExpr) has already consumed
//     the opening '<' token
//   - This function starts immediately after the '<'
// 
// ─── Token Consumption ─────────────────────────────────────────────────────
//   - If next token is '>', consumes it and returns empty span
//   - Otherwise parses comma‑separated types until '>'
//   - Consumes the closing '>'
// 
// ─── Loop Safety ──────────────────────────────────────────────────────────
//   - Uses saved position pattern with parseType()
//   - If parseType() makes no progress, consumes token and breaks
// 
// ─── Error Recovery ───────────────────────────────────────────────────────
//   - Missing '>' at end: consume() reports error
//   - Missing type after comma: reports error, breaks loop
//   - Empty list '<' '>' is valid (returns empty span)
// 
// ─── Return Value ─────────────────────────────────────────────────────────
//   Returns ArenaSpan<TypePtr> (temporary, caller converts to span)
// ============================================================================

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
