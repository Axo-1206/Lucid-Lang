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
// ArrayKind
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Distinguishes the three array types in Luc.
 *
 * - Slice   : non‑owning view (`[_, T]`)
 * - Dynamic : heap‑owned, growable (`[*, T]`)
 * - Fixed   : stack/inline, compile‑time size (`[N, T]`)
 */
enum class ArrayKind {
    Slice,
    Dynamic,
    Fixed
};

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
// ArrayTypeAST – unified concrete array type (slice, dynamic, fixed).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents a concrete array type: slice, dynamic, or fixed.
 *
 * This node unifies the three array kinds under a single representation.
 * The `kind` field determines which memory model applies.
 *
 * Grammar:
 *   array_type := '[' '_' ',' type ']'      -- slice
 *               | '[' '*' ',' type ']'      -- dynamic
 *               | '[' INT_LITERAL ',' type ']' -- fixed
 *
 * Examples:
 *   [_, int]   → kind = Slice,   element = Int
 *   [*, float] → kind = Dynamic, element = Float
 *   [4, Vec2]  → kind = Fixed,   size = 4, element = Vec2
 *
 * @note For arrays with a free type variable (e.g., `impl [_, <T>]`), use
 *       `GenericArrayTypeAST` instead.
 *
 * @field kind    The array kind (Slice, Dynamic, Fixed).
 * @field size    Only valid when `kind == Fixed`; ignored otherwise (should be 0).
 * @field element The element type (may itself be an array, function type, etc.).
 */
struct ArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayType;

    ArrayKind arrayKind;
    uint64_t size; // valid only for Fixed
    TypePtr element;

    ArrayTypeAST(ArrayKind k, uint64_t sz, TypePtr elem)
        : TypeAST(ASTKind::ArrayType), arrayKind(k), size(sz), element(std::move(elem)) {}

    bool isFixed()   const { return arrayKind == ArrayKind::Fixed; }
    bool isSlice()   const { return arrayKind == ArrayKind::Slice; }
    bool isDynamic() const { return arrayKind == ArrayKind::Dynamic; }
};

// ─────────────────────────────────────────────────────────────────────────────
// GenericArrayTypeAST – array with a free type variable (valid only as impl target)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Represents an array type with a free type variable, e.g., `impl [_, <T>]`.
 *
 * This node is only valid as the `impl_target` in an `impl` declaration. It
 * declares a type variable (e.g., `T`) that is bound to the concrete element
 * type of the array at the call site.
 *
 * Grammar:
 *   generic_array_type := '[' '_' ',' '<' IDENTIFIER '>' ']'      -- slice
 *                       | '[' '*' ',' '<' IDENTIFIER '>' ']'      -- dynamic
 *                       | '[' INT_LITERAL ',' '<' IDENTIFIER '>' ']' -- fixed
 *
 * Example:
 *   impl [*, <T>] as a {
 *       first () -> T = { return a[0] }
 *   }
 *
 * The variable `T` is in scope inside the method bodies of this `impl` block.
 * The semantic pass unifies `T` with the concrete element type of the array
 * the method is called on (e.g., `[*, int]` → `T = int`).
 *
 * @note This node does **not** represent a concrete array type. For concrete
 *       arrays (e.g., `impl [_, int]`), use `ArrayTypeAST`.
 *
 * @field kind          The kind of array: Slice, Dynamic, or Fixed.
 * @field size          For fixed arrays, the compile‑time size; ignored for others.
 * @field typeParamName The name of the free type variable (e.g., "T").
 */
struct GenericArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::GenericArrayType;

    ArrayKind arrayKind;
    uint64_t size; // valid only for Fixed
    InternedString typeParamName;

    // semantic
    bool isConst = false; // true for compile‑time constants

    GenericArrayTypeAST(ArrayKind k, uint64_t sz, InternedString name)
        : TypeAST(ASTKind::GenericArrayType),
          arrayKind(k), size(sz), typeParamName(name) {}

    bool isFixed()   const { return arrayKind == ArrayKind::Fixed; }
    bool isSlice()   const { return arrayKind == ArrayKind::Slice; }
    bool isDynamic() const { return arrayKind == ArrayKind::Dynamic; }
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
 *   - `#ptrDiff(p1, p2) -> int64` (distance in pointers)
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
    ArenaSpan<ParamAST*> allParams; // raw pointers
    ArenaSpan<size_t>   groupSizes; 
    ArenaSpan<TypeAST*> returnTypes; // raw pointers

    FuncSignature() = default;

    // Copy disabled, move enabled
    FuncSignature(const FuncSignature&) = delete;
    FuncSignature& operator=(const FuncSignature&) = delete;
    FuncSignature(FuncSignature&&) = default;
    FuncSignature& operator=(FuncSignature&&) = default;

    bool hasParams() const { return !allParams.empty(); }
    size_t totalParamCount() const { return allParams.size(); }
    size_t groupCount() const { return groupSizes.size(); }

    ArenaSpan<ParamAST*> getGroup(size_t idx) const {
        if (idx >= groupSizes.size()) return {};
        size_t offset = 0;
        for (size_t i = 0; i < idx; ++i) offset += groupSizes[i];
        return ArenaSpan<ParamAST*>(allParams.data() + offset, groupSizes[idx]);
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