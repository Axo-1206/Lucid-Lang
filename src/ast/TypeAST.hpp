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
 * @grammar (from LUC_GRAMMAR.md)
 *   type            := base_type [ generic_args ] [ '?' ]
 *                    | ref_type | ptr_type | array_type | func_type
 *   base_type       := primitive_type | IDENTIFIER
 *   primitive_type  := 'bool' | 'byte' | 'short' | 'int' | 'long'
 *                    | 'ubyte' | 'ushort' | 'uint' | 'ulong'
 *                    | 'int8' | 'int16' | 'int32' | 'int64'
 *                    | 'uint8' | 'uint16' | 'uint32' | 'uint64'
 *                    | 'float' | 'double' | 'decimal'
 *                    | 'string' | 'char' | 'any'
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

    void accept(ASTVisitor& v) override { v.visit(*this); }
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

    InternedString name;                     ///< Type name (e.g., "Vec2", "Buffer")
    ArenaSpan<TypePtr> genericArgs;          ///< Concrete type arguments (empty if non‑generic)

    // Semantic annotation (written by TypeResolver, read by codegen)
    bool isGenericParam = false;             ///< True if this is a generic parameter (T, K, V)

    explicit NamedTypeAST(InternedString n)
        : TypeAST(ASTKind::NamedType), name(n) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
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
 *   - `?.` chain operator is only valid on nullable targets(a non-nullable type can be used but will result a warning)
 *   - Every `?.` chain must be terminated by `??`
 *   - `?` is **not** valid on inline function types – use a type alias
 */
struct NullableTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NullableType;

    TypePtr inner;   ///< The type being made nullable

    explicit NullableTypeAST(TypePtr t)
        : TypeAST(ASTKind::NullableType), inner(std::move(t)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// FixedArrayTypeAST – compile‑time fixed‑size array `[N]T`.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief An array with a compile‑time constant size. Size is part of the type.
 *
 * @example
 *   [4]float    → size = 4, element = Float
 *   [16]float   → size = 16, element = Float
 *   [4][4]float → size = 4, element = FixedArrayTypeAST(4, Float)
 *
 * The size is stored as `uint64_t` from `INT_LITERAL`, always non‑negative.
 * The semantic pass checks that `size > 0` and fits platform limits.
 * Memory is allocated inline (stack or struct field).
 */
struct FixedArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FixedArrayType;

    std::uint64_t size;      ///< Compile‑time constant from `INT_LITERAL`
    TypePtr       element;   ///< Element type (may itself be an array)

    FixedArrayTypeAST(std::uint64_t sz, TypePtr elem)
        : TypeAST(ASTKind::FixedArrayType), size(sz), element(std::move(elem)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// SliceTypeAST – non‑owning view `[]T`.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A non‑owning view into an existing array. Internally a fat pointer.
 *
 * @example
 *   []int        → element = Int
 *   []Vec2       → element = NamedTypeAST("Vec2")
 *   [][*]float   → element = DynamicArrayTypeAST(element = Float)
 *
 * Slice expressions (`nums[1..3]`) produce a `SliceTypeAST` as their resolved type.
 * The slice shares memory with the original array – writing through the slice
 * affects the original, but reassigning the slice variable does not.
 */
struct SliceTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::SliceType;

    TypePtr element;   ///< Element type

    explicit SliceTypeAST(TypePtr elem)
        : TypeAST(ASTKind::SliceType), element(std::move(elem)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ─────────────────────────────────────────────────────────────────────────────
// DynamicArrayTypeAST – heap‑owned, growable array `[*]T`.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A heap‑owned, growable array.
 *
 * @example
 *   [*]int      → element = Int
 *   [*]Vec2     → element = NamedTypeAST("Vec2")
 *   [*][*]float → element = DynamicArrayTypeAST(element = Float)
 *
 * Semantic rules:
 *   - Mutating methods (`.push()`, `.pop()`, `.insert()`, `.remove()`, `.clear()`, `.reserve()`)
 *     are only valid when the variable is declared with `let`
 *   - Concatenation with `+` produces a new `[*]T`
 */
struct DynamicArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::DynamicArrayType;

    TypePtr element;   ///< Element type

    explicit DynamicArrayTypeAST(TypePtr elem)
        : TypeAST(ASTKind::DynamicArrayType), element(std::move(elem)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
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

    void accept(ASTVisitor& v) override { v.visit(*this); }
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

    TypePtr inner;   ///< The pointed‑to type

    explicit PtrTypeAST(TypePtr t)
        : TypeAST(ASTKind::PtrType), inner(std::move(t)) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};

// ═════════════════════════════════════════════════════════════════════════════
// FUNCTION SIGNATURE
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @brief Stores function signature data using flat arrays for cache efficiency.
 *
 * Parameter groups (currying) are stored as flattened arrays:
 *   - `allParams`  – flattened list of all parameters across all groups
 *   - `groupSizes` – size of each group (sum equals `allParams.size()`)
 *
 * @example
 *   (a int)(b int)(c int) → allParams = [a, b, c], groupSizes = [1, 1, 1]
 *   (x int, y int)        → allParams = [x, y],    groupSizes = [2]
 *
 * Qualifiers (`~async`, `~nullable`, `~parallel`) are part of the function
 * type for `~async` and `~nullable` – they affect type identity.
 * `~parallel` does not affect type identity.
 */
struct FuncSignature {
    ArenaSpan<ParamPtr> allParams;          ///< Flattened parameters across all groups
    ArenaSpan<size_t>   groupSizes;         ///< Size of each curry group

    ArenaSpan<TypePtr>  returnTypes;        ///< Return types (empty = void)

    uint32_t qualifiers = 0;                ///< `QualifierBits` flags
    ArenaSpan<InternedString> rawQualifiers; ///< Source qualifier strings (for error messages)

    FuncSignature() = default;

    // Copy disabled (ArenaSpan is trivially copyable, but ownership semantics are explicit)
    FuncSignature(const FuncSignature&) = delete;
    FuncSignature& operator=(const FuncSignature&) = delete;

    // Move enabled
    FuncSignature(FuncSignature&&) = default;
    FuncSignature& operator=(FuncSignature&&) = default;

    /// Check if a specific qualifier bit is set
    bool hasQualifier(uint32_t bit) const { return (qualifiers & bit) != 0; }

    /// True if the function is marked `~async`
    bool isAsync()    const { return hasQualifier(QualifierBits::Async); }

    /// True if the function is marked `~parallel` (implementation attribute)
    bool isParallel() const { return hasQualifier(QualifierBits::Parallel); }

    /// True if the function binding is `~nullable`
    bool isNullable() const { return hasQualifier(QualifierBits::Nullable); }

    /// True if the function has any parameters (across all groups)
    bool hasParams() const { return !allParams.empty(); }

    /// Total number of parameters across all groups
    size_t totalParamCount() const { return allParams.size(); }

    /// Number of curry groups
    size_t groupCount() const { return groupSizes.size(); }

    /**
     * @brief Get a specific parameter group as a span.
     * @param idx Group index (0‑based)
     * @return ArenaSpan containing the parameters in that group, or empty if index is out of range
     */
    ArenaSpan<ParamPtr> getGroup(size_t idx) const {
        if (idx >= groupSizes.size()) return {};

        size_t offset = 0;
        for (size_t i = 0; i < idx; ++i) {
            offset += groupSizes[i];
        }
        return ArenaSpan<ParamPtr>(allParams.data() + offset, groupSizes[idx]);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// FUNCTION TYPE
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @brief Represents a function type (e.g., in a type annotation).
 *
 * @example
 *   let callback (int) -> string = ...
 *
 * This node is used for function **types** in annotations. Declarations
 * (`FuncDeclAST`, `MethodDeclAST`, etc.) embed `FuncSignature` directly
 * to avoid an extra level of indirection.
 *
 * Function types include qualifiers (`~async`, `~nullable`) as part of the
 * type identity. `~parallel` is an implementation attribute and does not
 * affect type identity.
 */
struct FuncTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FuncType;

    FuncSignature sig;   ///< The function signature (params, returns, qualifiers)

    explicit FuncTypeAST() : TypeAST(ASTKind::FuncType) {}

    void accept(ASTVisitor& v) override { v.visit(*this); }
};