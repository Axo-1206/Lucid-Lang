/**
 * @file TypeAST.hpp
 *
 * @responsibility Defines the syntactic representation of types (Primitive, Array, Pointer, Function).
 *
 * @hierarchy BaseAST → TypeAST → [Concrete Nodes]
 *
 * @related_files
 *   - src/parser/ParserType.cpp – primary producer of these nodes
 *   - src/semantic/TypeResolver.cpp – resolves types to semantic representations
 *
 * @note These represent types **as written** in source. The semantic pass later
 *       resolves these into actual resolved Type objects.
 *
 */

#pragma once

#include "BaseAST.hpp"
#include "registry/QualifierRegistry.hpp"
#include "support/ArenaSpan.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveKind – all built‑in primitive types.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Identifies a primitive type in the type system.
 *
 * The parser maps token types (e.g., `TYPE_INT`) to this enum.
 * The semantic pass and codegen read `PrimitiveKind` directly.
 *
 * @note Fixed‑width types (`int8`, `uint32`, etc.) are critical for
 *       Vulkan struct layouts and FFI compatibility.
 */
enum class PrimitiveKind {
    // Boolean
    Bool,

    // Signed integers (machine‑dependent sizes)
    Byte,     // int8,  -128..127
    Short,    // int16
    Int,      // int32
    Long,     // int64

    // Unsigned integers (machine‑dependent sizes)
    Ubyte,    // uint8,  0..255
    Ushort,   // uint16
    Uint,     // uint32
    Ulong,    // uint64

    // Fixed‑width aliases – critical for Vulkan struct layouts
    Int8,
    Int16,
    Int32,
    Int64,
    Uint8,
    Uint16,
    Uint32,
    Uint64,

    // Floating point
    Float,    // 32‑bit
    Double,   // 64‑bit
    Decimal,  // 128‑bit, high precision

    // Text
    String,
    Char,

    // Dynamic type – accepts any value, resolved at runtime
    Any,
};

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveTypeAST – a built‑in primitive keyword used as a type.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a primitive type keyword.
 *
 * @example
 *   let x int    = 5       → PrimitiveKind::Int
 *   let s string = "hi"    → PrimitiveKind::String
 *   let v any    = getData() → PrimitiveKind::Any
 */
struct PrimitiveTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PrimitiveType;

    PrimitiveKind primitiveKind;   ///< Which primitive type

    explicit PrimitiveTypeAST(PrimitiveKind k)
        : TypeAST(ASTKind::PrimitiveType), primitiveKind(k) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// NamedTypeAST – a user‑defined type referenced by name.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief References a user‑defined type by name, with optional generic arguments.
 *
 * @example
 *   Vec2               → name = "Vec2",    genericArgs = {}
 *   Buffer<int>        → name = "Buffer",  genericArgs = [Int]
 *   Map<string, Vec2>  → name = "Map",     genericArgs = [String, Vec2]
 *
 * `genericArgs` holds the concrete types supplied at the use site (e.g., the
 * `<int>` in `Buffer<int>`). These are `TypeAST` nodes, not `GenericParamAST`.
 * The semantic pass resolves the name against the symbol table and verifies
 * the argument count matches the declaration.
 *
 * ## Semantic Annotation: `isGenericParam`
 *
 * Set to `true` by `TypeResolver` when the name matches a generic type parameter
 * declared on the enclosing declaration (e.g., `T` in `struct Box<T>` or `let process<T>`).
 * This distinguishes abstract parameters like `T` from concrete types like `Circle`.
 *
 * Codegen uses this flag to skip instantiation collection for abstract uses –
 * `InstKey{"Box", ["T"]}` is meaningless and must not be recorded.
 */
struct NamedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NamedType;

    InternedString name;            // Type name (e.g., "Vec2", "Buffer")
    ArenaSpan<TypePtr> genericArgs; // Concrete type arguments (empty if non‑generic)

    // Semantic annotation (written by TypeResolver, read by codegen)
    bool isGenericParam = false;    // True if this is a generic parameter (T, K, V)

    explicit NamedTypeAST(InternedString n)
        : TypeAST(ASTKind::NamedType), name(n) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// NullableTypeAST – the `?` suffix for nullable value types.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Wraps an inner type with the nullable suffix `?`.
 *
 * @example
 *   int?                       → inner = PrimitiveTypeAST(Int)
 *   Vec2?                      → inner = NamedTypeAST("Vec2")
 *   ~nullable (int) -> string? → inner = FuncTypeAST(...)
 *
 * Grammar rules enforced by the semantic pass:
 *   - `?` attaches to value types only (primitives, structs, arrays, named aliases)
 *   - `?.` chain operator is only valid on nullable targets (a non-nullable type can be used but will result in a warning)
 *   - Every `?.` chain must be terminated by `??`
 *   - `?` is **not** valid on inline function types – use a type alias
 */
struct NullableTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NullableType;

    TypePtr inner;   ///< The type being made nullable

    explicit NullableTypeAST(TypePtr t)
        : TypeAST(ASTKind::NullableType), inner(std::move(t)) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// ResultTypeAST – the `!` suffix: success type T paired with error type E.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Wraps a success type with an optional error type using the `!` suffix.
 *
 * @example
 *   int!string   → inner = PrimitiveTypeAST(Int),  errorType = PrimitiveTypeAST(String)
 *   int!         → inner = PrimitiveTypeAST(Int),  errorType = nullptr  (bare '!')
 *   int?!string  → inner = NullableTypeAST(Int),   errorType = PrimitiveTypeAST(String)
 *   int?!        → inner = NullableTypeAST(Int),   errorType = nullptr
 *
 * Grammar rules enforced by the semantic pass:
 *   - Neither `inner` nor `errorType` may itself be a ResultTypeAST
 *     (nesting '!' is forbidden — see §Nesting `!` is Forbidden in grammar)
 *   - `?` always comes before `!` when both are present (inner is NullableTypeAST)
 *   - `!` is NEVER valid directly after an array type or inline function type —
 *     use a named alias first (same rule as `?`)
 */
struct ResultTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::ResultType;

    TypePtr inner;       ///< The success type (T in T!E or T?!E)
    TypePtr errorType;   ///< The error type E; nullptr means bare '!' (fails with nil)

    ResultTypeAST(TypePtr t, TypePtr err)
        : TypeAST(ASTKind::ResultType),
          inner(std::move(t)), errorType(std::move(err)) {}

    /// Convenience: true when this is a bare '!' with no error payload
    bool hasErrorType() const { return errorType != nullptr; }

    // Makes the semantic pass cleaner and provides a single place to enforce the grammar rule.
    bool isWellFormed() const {
        if (inner && inner->isa<ResultTypeAST>()) return false;
        if (errorType && errorType->isa<ResultTypeAST>()) return false;
    return true;
}
};

// ─────────────────────────────────────────────────────────────────────────────
// FixedArrayTypeAST – compile‑time fixed‑size array `[N]T`.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An array with a compile‑time constant size. Size is part of the type.
 *
 * @example
 *   [4, float]         → size = 4, element = Float
 *   [16, float]        → size = 16, element = Float
 *   [4, [4, float] ]   → size = 4, element = FixedArrayTypeAST(4, Float)
 *
 * The size is stored as `uint64_t` from `INT_LITERAL`, always non‑negative.
 * The semantic pass checks that `size > 0` and fits platform limits.
 * Memory is allocated inline (stack or struct field).
 */
struct FixedArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FixedArrayType;

    std::uint64_t size;    // Compile‑time constant from `INT_LITERAL`
    TypePtr       element; // Element type (may itself be an array)

    FixedArrayTypeAST(std::uint64_t sz, TypePtr elem)
        : TypeAST(ASTKind::FixedArrayType), size(sz), element(std::move(elem)) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// SliceTypeAST – non‑owning view `[]T`.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A non‑owning view into an existing array. Internally a fat pointer.
 *
 * @example
 *   [_, int]           → element = Int
 *   [_, Vec2]          → element = NamedTypeAST("Vec2")
 *   [_, [*, float] ]   → element = DynamicArrayTypeAST(element = Float)
 *
 * Slice expressions (`nums[1..3]`) produce a `SliceTypeAST` as their resolved type.
 * The slice shares memory with the original array – writing through the slice
 * affects the original, but reassigning the slice variable does not.
 */
struct SliceTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::SliceType;

    TypePtr element; // Element type

    explicit SliceTypeAST(TypePtr elem)
        : TypeAST(ASTKind::SliceType), element(std::move(elem)) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// DynamicArrayTypeAST – heap‑owned, growable array `[*]T`.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A heap‑owned, growable array.
 *
 * @example
 *   [*, int]           → element = Int
 *   [*, Vec2]          → element = NamedTypeAST("Vec2")
 *   [*, [*, float] ]   → element = DynamicArrayTypeAST(element = Float)
 *
 * Semantic rules:
 *   - Mutating methods (`.push()`, `.pop()`, `.insert()`, `.remove()`, `.clear()`, `.reserve()`)
 *     are only valid when the variable is declared with `let`
 *   - Concatenation with `+` produces a new `[*]T`
 */
struct DynamicArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::DynamicArrayType;

    TypePtr element; // Element type

    explicit DynamicArrayTypeAST(TypePtr elem)
        : TypeAST(ASTKind::DynamicArrayType), element(std::move(elem)) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// RefTypeAST – safe managed reference `&T`.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A safe managed reference to another value.
 *
 * @example
 *   &int    → inner = PrimitiveTypeAST(Int)
 *   &Vec2   → inner = NamedTypeAST("Vec2")
 *
 * References are always valid (non‑nullable by default). To express a nullable
 * reference, wrap in `NullableTypeAST`: `&Vec2?`.
 *
 * Used for struct fields that point to other structs (linked lists, trees)
 * and for passing large values by reference without copying.
 */
struct RefTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::RefType;

    TypePtr inner;   ///< The referenced type

    explicit RefTypeAST(TypePtr t)
        : TypeAST(ASTKind::RefType), inner(std::move(t)) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// PtrTypeAST – raw, unmanaged pointer `*T` (sealed conduit).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A raw, unmanaged pointer – the **sealed conduit**.
 *
 * ## The Sealed Conduit Model
 *
 * Raw pointers (`*T`) are sealed conduits. You can carry them, pass them to
 * `@extern` functions, check for nil, but never dereference directly.
 *
 * **Allowed operations:**
 * 1. Store in a variable, struct field, or parameter
 * 2. Pass to an `@extern` function
 * 3. Nil check (`== nil`, `!= nil`)
 * 4. Pass to pointer intrinsics (`#ptrToRef`, `#ptrOffset`, etc.)
 * 5. Print the address for debugging
 *
 * **Forbidden operations (compiler error):**
 *   - Dereference: `*ptr`
 *   - Field access: `ptr.field`
 *   - Indexing: `ptr[i]`
 *   - Arithmetic: `ptr + 4` – use `#ptrOffset` instead
 *   - Assignment: `*ptr = value`
 *
 * **Boundary crossing (intrinsics):**
 *   - `#ptrToRef(ptr) -> &T`   (cross to safe reference)
 *   - `#refToPtr(ref) -> *T`   (convert back to raw pointer)
 *   - `#ptrOffset(ptr, n) -> *T` (pointer arithmetic)
 *   - `#ptrDiff(p1, p2) -> int64` (distance in elements)
 *
 * **Valid contexts for `PtrTypeAST`:**
 *   - `@extern`-decorated declarations
 *   - Input/output types of pointer‑related intrinsics
 *   - Variables/parameters holding values returned by `@extern` / intrinsics
 */
struct PtrTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PtrType;

    TypePtr inner; // The pointed‑to type

    explicit PtrTypeAST(TypePtr t)
        : TypeAST(ASTKind::PtrType), inner(std::move(t)) {}

};

// ─────────────────────────────────────────────────────────────────────────────
// FUNCTION SIGNATURE (pure parameter/return shape, no qualifiers)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Pure signature of a function: parameters (flattened with curry groups)
 *        and return types. Does NOT include qualifiers (`~async`, `~nullable`,
 *        `~parallel`).
 *
 * This struct is reused in multiple contexts:
 *   - `FuncDeclAST` / `MethodDeclAST` / `TraitMethodAST` – combined with
 *     separate qualifier fields to form a complete declaration.
 *   - `AnonFuncExprAST` – anonymous functions have no qualifiers, so only the
 *     pure signature is needed.
 *   - `FuncTypeAST` – combined with qualifier fields to form a complete
 *     function type (used in type annotations).
 *
 * @see FuncTypeAST for the qualifier‑bearing version.
 */
struct FuncSignature {
    ArenaSpan<ParamPtr> allParams;          // Flattened parameters across all groups
    ArenaSpan<size_t>   groupSizes;         // Size of each curry group
    ArenaSpan<TypePtr>  returnTypes;        // Return types (empty = void)

    FuncSignature() = default;

    // Copy disabled, move enabled
    FuncSignature(const FuncSignature&) = delete;
    FuncSignature& operator=(const FuncSignature&) = delete;
    FuncSignature(FuncSignature&&) = default;
    FuncSignature& operator=(FuncSignature&&) = default;

    bool hasParams() const { return !allParams.empty(); }
    size_t totalParamCount() const { return allParams.size(); }
    size_t groupCount() const { return groupSizes.size(); }

    ArenaSpan<ParamPtr> getGroup(size_t idx) const {
        if (idx >= groupSizes.size()) return {};
        size_t offset = 0;
        for (size_t i = 0; i < idx; ++i) offset += groupSizes[i];
        return ArenaSpan<ParamPtr>(allParams.data() + offset, groupSizes[idx]);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// FUNCTION TYPE
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Complete function type, combining a pure signature with qualifiers
 *        (`~async`, `~nullable`, `~parallel`).
 *
 * Used in type annotations (e.g., `let f : ~async (int) -> string`). The
 * qualifiers are part of the type identity for `~async` and `~nullable`;
 * `~parallel` is an implementation attribute that does not affect type
 * equality.
 *
 * @note Anonymous function expressions (`AnonFuncExprAST`) never have qualifiers
 *       – they are plain values. The qualifier context comes from the
 *       declaration or parameter type to which the anonymous function is assigned.
 */
struct FuncTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FuncType;

    FuncSignature sig;                         // pure parameter/return shape
    uint32_t qualifiers = 0;                   // QualifierBits flags
    ArenaSpan<InternedString> rawQualifiers;   // source qualifier strings (for diagnostics)

    explicit FuncTypeAST() : TypeAST(ASTKind::FuncType) {}

    bool hasQualifier(uint32_t bit) const { return (qualifiers & bit) != 0; }
    bool isAsync()    const { return hasQualifier(QualifierBits::Async); }
    bool isParallel() const { return hasQualifier(QualifierBits::Parallel); }
    bool isNullable() const { return hasQualifier(QualifierBits::Nullable); }
};