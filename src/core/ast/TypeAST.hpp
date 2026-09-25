/// @file TypeAST.hpp
/// 
/// @responsibility Defines the syntactic representation of types
///                 (Primitive, Array, Reference, Function, Nullable, Fallible).
/// 
/// @hierarchy BaseAST → TypeAST → [Concrete Nodes]
/// 
/// @related_files
///   - src/parser/ParserType.cpp – primary producer of these nodes
///   - src/semantic/TypeResolver.cpp – resolves types to semantic representations
/// 
/// @note These represent types **as written** in source. The semantic pass
///       resolves them into fully resolved semantic types.
///
/// ─── No Removed Nodes ────────────────────────────────────────────────────
/// This header deliberately has NO nodes for:
///   - Raw pointers (`*T`) — removed from the language.
///   - `Future<T>` / `Thread<T>` — replaced by the single `Deferred<T>` type,
///     which is a host-backed `NamedTypeAST`, not a dedicated node.
///   - `Arena` / `ArenaDescriptor` — no longer boot-level; they may be declared
///     as host-backed types in a core script (`TYPE Arena = #host(...)`) and
///     use the ordinary `NamedTypeAST` shape.
///   - `Simd<T, N>` — a core-script type (`TYPE Simd<T, N> = #builtin(simd_type)`)
///     resolved as a `NamedTypeAST`. Named aliases like `Float4` are also
///     `NamedTypeAST`s.
///   - `ModuleTypeAccess` — `::` is handled through `ModuleAccessExprAST` on the
///     value side; module-qualified type names resolve through the ordinary
///     named-type path.

#pragma once

#include "BaseAST.hpp"
#include "core/memory/StringPool.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// ArrayKind — the three array shapes.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Distinguishes the three array types in Lucid.
/// 
/// - Slice   : non‑owning view (`[_]T`)
/// - Dynamic : heap‑owned, growable (`[*]T`)
/// - Fixed   : stack/inline, compile‑time size (`[N]T`)
enum class ArrayKind {
    Slice,   // [_]T
    Dynamic, // [*]T
    Fixed    // [N]T
};

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveKind — the primitive type tags.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Identifies a primitive type in the type system.
/// 
/// The parser maps token types (e.g., `TYPE_INT`) to this enum.
/// The semantic pass and codegen read `PrimitiveKind` directly.
/// 
/// @note Fixed‑width types (`int8`, `uint32`, etc.) are critical for
///       Vulkan struct layouts and FFI compatibility.
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
};

// ─── PrimitiveKind Predicates ─────────────────────────────────────────────

/// @brief Get the bit width of a primitive kind.
inline size_t getPrimitiveBitWidth(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Bool:
        case PrimitiveKind::Char:
        case PrimitiveKind::Byte:
        case PrimitiveKind::Ubyte:
        case PrimitiveKind::Int8:
        case PrimitiveKind::Uint8:
            return 8;

        case PrimitiveKind::Short:
        case PrimitiveKind::Ushort:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Uint16:
            return 16;

        case PrimitiveKind::Int:
        case PrimitiveKind::Uint:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Uint32:
            return 32;

        case PrimitiveKind::Long:
        case PrimitiveKind::Ulong:
        case PrimitiveKind::Int64:
        case PrimitiveKind::Uint64:
            return 64;

        case PrimitiveKind::Float:
        case PrimitiveKind::Double:
        case PrimitiveKind::Decimal:
        case PrimitiveKind::String:
            return 0;

        default:
            return 0;
    }
}

inline bool isSignedIntegerKind(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Byte:
        case PrimitiveKind::Short:
        case PrimitiveKind::Int:
        case PrimitiveKind::Long:
        case PrimitiveKind::Int8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int64:
            return true;
        default:
            return false;
    }
}

inline bool isUnsignedIntegerKind(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Ubyte:
        case PrimitiveKind::Ushort:
        case PrimitiveKind::Uint:
        case PrimitiveKind::Ulong:
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint64:
            return true;
        default:
            return false;
    }
}

inline bool isFloatKind(PrimitiveKind kind) {
    switch (kind) {
        case PrimitiveKind::Float:
        case PrimitiveKind::Double:
        case PrimitiveKind::Decimal:
            return true;
        default:
            return false;
    }
}

inline bool isIntegerKind(PrimitiveKind kind) {
    return isSignedIntegerKind(kind) || isUnsignedIntegerKind(kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a primitive type keyword.
/// 
/// @example
///   let x int    = 5       → PrimitiveKind::Int
///   let s string = "hi"    → PrimitiveKind::String
///   let b bool   = true    → PrimitiveKind::Bool
struct PrimitiveTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PrimitiveType;

    PrimitiveKind primitiveKind;

    explicit PrimitiveTypeAST(PrimitiveKind k)
        : TypeAST(ASTKind::PrimitiveType), primitiveKind(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NamedTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief References a user‑defined type by name, with optional generic arguments.
/// 
/// @example
///   Vec2               → name = "Vec2",    genericArgs = {}
///   Buffer<int>        → name = "Buffer",  genericArgs = [Int]
///   Map<string, Vec2>  → name = "Map",     genericArgs = [String, Vec2]
///   Deferred<User>     → name = "Deferred", genericArgs = [User]
///   Weak<Node>         → name = "Weak",     genericArgs = [Node]
///   Float4             → name = "Float4",   genericArgs = {}
/// 
/// `genericArgs` holds the concrete types supplied at the use site (e.g., the
/// `<int>` in `Buffer<int>`). These are `TypeAST` nodes, not `GenericParamAST`.
/// The semantic pass resolves the name against the symbol table and verifies
/// the argument count matches the declaration.
/// 
/// ─── Resolution Targets ─────────────────────────────────────────────────
/// A `NamedTypeAST` may resolve to any of:
///   - `StructDeclAST` — a user-defined struct (possibly a specialization).
///   - `EnumDeclAST` — a user-defined enum (possibly a specialization).
///   - `HostTypeDeclAST` — a host-backed type (`Map`, `Deferred`, `Weak`,
///     `Simd`, `Float4`, ...).
///   - `TypeAliasDeclAST` — a type alias.
///   - `GenericParamDeclAST` — a generic parameter reference (`T`, `K`, ...).
///   - `TraitDeclAST` — only in constraint position (`<T : Trait>`).
/// The resolved decl is written to `resolvedDecl` by Sema.
struct NamedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NamedType;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    InternedString name;
    ArenaSpan<TypeAST*> genericArgs;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// @brief The resolved declaration for this named type.
    ///
    /// Set by `resolveNamedType()` during semantic analysis. See the class
    /// comment above for the full set of possible targets.
    TypeDeclAST* resolvedDecl = nullptr;

    explicit NamedTypeAST(InternedString n)
        : TypeAST(ASTKind::NamedType), name(n) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NullableTypeAST / FallibleTypeAST / CombinedTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Wraps an inner type with the nullable suffix `?`.
/// 
/// @example
///   int?        → inner = PrimitiveTypeAST(Int)
///   Vec2?       → inner = NamedTypeAST("Vec2")
///   User?       → inner = NamedTypeAST("User")
/// 
/// Grammar rules enforced by the semantic pass:
///   - `?` attaches to value types only (primitives, structs, enums).
///   - `?` binds to the **element type** of an array, not the array type:
///     `[*]int?` is an array of nullable ints. There is no nullable array
///     type; use an empty array to signal "no array".
///   - `?` is not valid on function types.
/// 
/// @see CombinedTypeAST for `T?!` (nullable + fallible combined)
struct NullableTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NullableType;

    TypeAST* inner = nullptr;

    explicit NullableTypeAST(TypeAST* t)
        : TypeAST(ASTKind::NullableType), inner(t) {}
};

/// @brief Wraps an inner type with the fallible suffix `!`.
/// 
/// @example
///   int!        → inner = PrimitiveTypeAST(Int)
///   string!     → inner = PrimitiveTypeAST(String)
///   User!       → inner = NamedTypeAST("User")
/// 
/// Grammar rules enforced by the semantic pass:
///   - `!` attaches to value types only (primitives, structs, enums).
///   - `!` binds to the **element type** of an array, not the array type:
///     `[*]int!` is an array of fallible ints.
///   - `!` is not valid on function types.
/// 
/// @see CombinedTypeAST for `T?!` (nullable + fallible combined)
struct FallibleTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FallibleType;

    TypeAST* inner = nullptr;

    explicit FallibleTypeAST(TypeAST* t)
        : TypeAST(ASTKind::FallibleType), inner(t) {}
};

/// @brief Represents a type that is both nullable and fallible: `T?!`
/// 
/// @example
///   int?!       → inner = PrimitiveTypeAST(Int)
///   User?!      → inner = NamedTypeAST("User")
/// 
/// A `T?!` value has three states. Narrowing must rule out both sentinels
/// before the plain `T` is usable.
/// 
/// Grammar rules enforced by the semantic pass:
///   - `?!` is the only valid order — `!?` is a parse error.
///   - Same restrictions as `?` and `!` individually apply.
/// 
/// @note This is a distinct type from `NullableTypeAST` + `FallibleTypeAST`
///       composition. The combined type has three states (T, nil, err) while
///       `T?` has two (T, nil) and `T!` has two (T, err).
struct CombinedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::CombinedType;

    TypeAST* inner = nullptr;

    explicit CombinedTypeAST(TypeAST* t)
        : TypeAST(ASTKind::CombinedType), inner(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// ArrayTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a concrete array type: slice, dynamic, or fixed.
/// 
/// This node unifies the three array kinds under a single representation.
/// The `kind` field determines which memory model applies.
/// 
/// Grammar:
///   array_type := '[' '*' ']' type      -- owned heap array
///               | '[' '_' ']' type      -- slice (borrowed view)
///               | '[' INT_LITERAL ']' type   -- fixed-size stack array
/// 
/// Examples:
///   [*]int   → kind = Dynamic, element = Int
///   [_]float → kind = Slice,   element = Float
///   [4]Vec2  → kind = Fixed,   size = 4, element = Vec2
/// 
/// @note `?` and `!` annotations apply to the element type, not the array
///       itself: `[*]int?` is an array of nullable int; there is no nullable
///       array type.
/// 
/// @field arrayKind  The array kind (Slice, Dynamic, Fixed).
/// @field size       Only valid when `arrayKind == Fixed`; ignored otherwise.
/// @field element    The element type.
struct ArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayType;

    ArrayKind arrayKind;
    uint64_t size;
    TypeAST* element = nullptr;

    ArrayTypeAST(ArrayKind k, uint64_t sz, TypeAST* elem)
        : TypeAST(ASTKind::ArrayType), arrayKind(k), size(sz), element(elem) {}

    bool isFixed()   const { return arrayKind == ArrayKind::Fixed; }
    bool isSlice()   const { return arrayKind == ArrayKind::Slice; }
    bool isDynamic() const { return arrayKind == ArrayKind::Dynamic; }
};

// ─────────────────────────────────────────────────────────────────────────────
// RefTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A strong reference to another value, written `&T`.
/// 
/// @example
///   &int    → inner = PrimitiveTypeAST(Int)
///   &Vec2   → inner = NamedTypeAST("Vec2")
/// 
/// References are refcounted. The referent's storage is kept alive as long as
/// at least one `&T` points at it. Storability is unrestricted:
///   - Struct fields may have reference type.
///   - Arrays and slices may store reference types.
///   - Functions may return reference types.
///   - Closures may capture reference values.
/// 
/// ─── Cycles ─────────────────────────────────────────────────────────────
/// A cycle of strong references keeps every value in the cycle alive. The
/// fix is to make one edge `Weak<T>` — a non-owning reference that does not
/// participate in refcounting. The compiler warns on obvious same-scope
/// cycles but does not enforce the fix statically; the rest is the user's
/// responsibility.
/// 
/// ─── Nullable References ────────────────────────────────────────────────
/// To express a nullable reference, wrap in `NullableTypeAST`: `&Vec2?`.
/// A recursive reference field (`next &Node`) must be nullable; without `?`,
/// the first instance of the struct cannot be constructed (there is no
/// existing instance for the field to point at).
struct RefTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::RefType;

    TypeAST* inner = nullptr;

    explicit RefTypeAST(TypeAST* t)
        : TypeAST(ASTKind::RefType), inner(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FuncTypeAST — function type.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A function type.
///
/// A function type is a chain of stages. Every stage is preceded by
/// `fn`:
///
///   func_type = stage { '->' stage } [ '->' type ]
///   stage     = 'fn' '(' [ type_list ] ')'
///
/// A stage's parameter group and return type describe the signature.
/// The runtime representation of a value of this type is uniform — a
/// fat pointer whose environment is null when the function captures
/// nothing and refcounted otherwise. The compiler tracks capture
/// behavior; the type does not.
///
/// The `params` here are the type-side parameters. When this
/// FuncTypeAST is part of a declaration header, the same shape is
/// mirrored on the declaration's AnonFuncExprAST, whose `params` are
/// the runtime parameters CodeGen binds. See `AnonFuncExprAST` for
/// the distinction.
struct FuncTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FuncType;

    ArenaSpan<ParamAST*> params;      // parameters for this group
    TypeAST* returnType = nullptr;     // return types (may contain FuncTypeAST)

    explicit FuncTypeAST() : TypeAST(ASTKind::FuncType) {}

    // Returns true if the return type is a function type (currying)
    bool isCurried() const {
        return returnType && returnType->isa<FuncTypeAST>();
    }

    // Returns the inner function type if curried, otherwise nullptr
    FuncTypeAST* getNext() const {
        if (isCurried()) {
            return returnType->as<FuncTypeAST>();
        }
        return nullptr;
    }
};