/// @file TypeAST.hpp
/// 
/// @responsibility Defines the syntactic representation of types (Primitive, Array, Pointer, Function).
/// 
/// @hierarchy BaseAST → TypeAST → [Concrete Nodes]
/// 
/// @related_files
///   - src/parser/ParserType.cpp – primary producer of these nodes
///   - src/semantic/TypeResolver.cpp – resolves types to semantic representations
/// 
/// @note These represent types **as written** in source. The semantic pass later
///       resolves these into actual resolved Type objects.

#pragma once

#include "BaseAST.hpp"
#include "core/memory/StringPool.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

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
// FuncShape — the runtime representation of a function value.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Distinguishes the two runtime shapes a function value can take.
///
/// Every function type stage is preceded by `fn` or `cls` in source. The
/// marker is mandatory and per-stage: in a curry chain, each parameter
/// group carries its own marker, so a single signature can mix shapes.
///
/// | Marker | Runtime value | Words | Resource?            | Call protocol |
/// | ------ | ------------- | ----- | -------------------- | ------------- |
/// | `fn`   | bare `ptr`    | 1     | No                   | direct call   |
/// | `cls`  | `{func, env}` | 2     | Yes (refcounted env) | closure call  |
///
/// The distinction is static. CodeGen knows which shape each function value
/// has from its type, so there is no runtime shape check and no conservative
/// retain/release: `fn` values skip ownership entirely, `cls` values follow
/// the existing ownership model.
enum class FuncShape : uint8_t {
    Fn,     ///< Bare function pointer. One word. No environment. Not a resource.
    Cls,    ///< Closure fat pointer `{func, env}`. Two words. Refcounted env.
};

// ─── PrimitiveKind ─────────────────────────────────────────────────────────

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

/// @brief Get the bit width of a primitive kind.
/// 
/// This is a property of the primitive kind itself, not of any particular
/// phase (Sema or CodeGen). It's defined here so both phases can share it.
/// 
/// @param kind The primitive kind.
/// @return The bit width (8, 16, 32, 64), or 0 if the kind is not an integer.
/// 
/// @note Bool and Char are both 8 bits wide when lowered to LLVM, but
///       Sema's type system treats them as distinct categories. This
///       function returns the width regardless of category.
inline size_t getPrimitiveBitWidth(PrimitiveKind kind) {
    switch (kind) {
        // ─── 8-bit ──────────────────────────────────────────────────────
        case PrimitiveKind::Bool:
        case PrimitiveKind::Char:
        case PrimitiveKind::Byte:
        case PrimitiveKind::Ubyte:
        case PrimitiveKind::Int8:
        case PrimitiveKind::Uint8:
            return 8;

        // ─── 16-bit ─────────────────────────────────────────────────────
        case PrimitiveKind::Short:
        case PrimitiveKind::Ushort:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Uint16:
            return 16;

        // ─── 32-bit ─────────────────────────────────────────────────────
        case PrimitiveKind::Int:
        case PrimitiveKind::Uint:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Uint32:
            return 32;

        // ─── 64-bit ─────────────────────────────────────────────────────
        case PrimitiveKind::Long:
        case PrimitiveKind::Ulong:
        case PrimitiveKind::Int64:
        case PrimitiveKind::Uint64:
            return 64;

        // ─── Non-integer types ──────────────────────────────────────────
        case PrimitiveKind::Float:
        case PrimitiveKind::Double:
        case PrimitiveKind::Decimal:
        case PrimitiveKind::String:
            return 0;

        default:
            return 0;
    }
}

/// @brief Check if a primitive kind is a signed integer type.
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

/// @brief Check if a primitive kind is an unsigned integer type.
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

/// @brief Check if a primitive kind is a floating-point type.
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

/// @brief Check if a primitive kind is an integer type (signed or unsigned).
inline bool isIntegerKind(PrimitiveKind kind) {
    return isSignedIntegerKind(kind) || isUnsignedIntegerKind(kind);
}

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

/// @brief References a user‑defined type by name, with optional generic arguments.
/// 
/// @example
///   Vec2               → name = "Vec2",    genericArgs = {}
///   Buffer<int>        → name = "Buffer",  genericArgs = [Int]
///   Map<string, Vec2>  → name = "Map",     genericArgs = [String, Vec2]
/// 
/// `genericArgs` holds the concrete types supplied at the use site (e.g., the
/// `<int>` in `Buffer<int>`). These are `TypeAST` nodes, not `GenericParamAST`.
/// The semantic pass resolves the name against the symbol table and verifies
/// the argument count matches the declaration.
/// 
/// @note Named types hold generic arguments, not generic parameters.
struct NamedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NamedType;

    // ─── Parser Fields (immutable) ──────────────────────────────────────────
    InternedString name;
    ArenaSpan<TypeAST*> genericArgs;
    
    // ─── Semantic Fields (set by Sema) ────────────────────────────────────
    /// @brief The resolved declaration for this named type.
    /// 
    /// This is set by `resolveNamedType()` during semantic analysis.
    /// It can be:
    ///   - For a generic instantiation: the specialized StructDeclAST
    ///     produced by resolveGenericInstantiation (e.g., Box_int).
    ///   - For a non-generic type: the original StructDeclAST / EnumDeclAST /
    ///     TraitDeclAST.
    ///
    /// A generic parameter reference (e.g. `T` inside `struct Box<T>`) is
    /// resolved to the GenericParamDeclAST itself; Sema treats that case
    /// separately from a concrete named type — see resolveNamedType.
    TypeDeclAST* resolvedDecl = nullptr;

    explicit NamedTypeAST(InternedString n)
        : TypeAST(ASTKind::NamedType), name(n) {}
};

/// @brief Wraps an inner type with the nullable suffix `?`.
/// 
/// @example
///   int?        → inner = PrimitiveTypeAST(Int)
///   Vec2?       → inner = NamedTypeAST("Vec2")
///   User?       → inner = NamedTypeAST("User")
/// 
/// Grammar rules enforced by the semantic pass:
///   - `?` attaches to value types only (primitives, structs, enums, traits)
///   - `?` is **not** valid on array types (`[*]int?`) — use `[*]int?` for
///     array of nullable elements, or `~nullable [*]int` for nullable array
///   - `?` is **not** valid on function types (`(int) -> bool?`)
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
///   - `!` attaches to value types only (primitives, structs, enums, traits)
///   - `!` is **not** valid on array types (`[*]int!`) — use `[*]int!` for
///     array of fallible elements
///   - `!` is **not** valid on function types (`(int) -> bool!`)
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
/// A `T?!` value is a genuine three-state value. Narrowing must rule out both
/// sentinels before the plain `T` is usable.
/// 
/// Grammar rules enforced by the semantic pass:
///   - `?!` is the only valid order — `!?` is rejected by the parser
///   - Same restrictions as `?` and `!` individually apply
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
/// @note `?` and `!` annotations apply to the element type, not the array itself:
///   `[*]int?`  → array of nullable int
///   `[*]int!`  → array of fallible int
/// 
/// @field kind    The array kind (Slice, Dynamic, Fixed).
/// @field size    Only valid when `kind == Fixed`; ignored otherwise (should be 0).
/// @field element The element type (may itself be an array, function type, etc.).
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

/// @brief A safe managed reference to another value.
/// 
/// @example
///   &int    → inner = PrimitiveTypeAST(Int)
///   &Vec2   → inner = NamedTypeAST("Vec2")
/// 
/// References are refcounted. Storability is unrestricted:
///   - Struct fields may have reference type (`&T`).
///   - Arrays and slices may store reference types.
///   - Functions may return reference types.
///   - Closures may capture reference values.
/// 
/// Cycles between reference-typed fields must use `Weak<T>` to break the cycle.
/// A `Weak<T>` is a non-owning reference that does not participate in
/// refcounting; accessing it requires a nil-check (it becomes nil when the
/// referent is freed).
/// 
/// To express a nullable reference, wrap in `NullableTypeAST`: `&Vec2?`.
struct RefTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::RefType;

    TypeAST* inner = nullptr;

    explicit RefTypeAST(TypeAST* t)
        : TypeAST(ASTKind::RefType), inner(t) {}
};



// ─────────────────────────────────────────────────────────────────────────────
// FuncTypeAST — function type.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Represents a function type with a single parameter group.
///
/// This is a recursive design: a function type consists of one parameter group
/// and one return type. If the function is curried, the return type
/// is another FuncTypeAST.
///
/// ─── Per-Stage Shape Markers (Mandatory) ───────────────────────────────
///
/// Every function type stage is preceded by `fn` or `cls`:
///
///   func_type = stage { '->' stage } [ '->' type ]
///   stage     = ( 'fn' | 'cls' ) unnamed_group
///
/// The marker is mandatory and applies to that stage only. **Every group
/// in a curry chain carries its own marker** — writing `fn (a int)(b int)
/// -> int` is malformed, because `(b int)` has no marker. The correct form
/// is `fn (a int) fn (b int) -> int`, where each group is explicitly
/// prefixed.
///
/// Groups may be adjacent (desugars to arrow) or arrow-separated. Adjacency
/// is purely a syntactic shorthand for "these groups form a curry chain";
/// it does **not** propagate a marker from one group to the next. The parser
/// desugars multiple parameter groups into nested FuncTypeAST nodes, and
/// each nested node carries its own `shape`.
///
/// Examples:
///
///   fn (a int) cls (b int) -> int
///     → outer: params=[a], shape=Fn,  returnType = inner
///     → inner: params=[b], shape=Cls, returnType = int
///
///   fn (a int) fn (b int) -> int
///     → outer: params=[a], shape=Fn,  returnType = inner
///     → inner: params=[b], shape=Fn,  returnType = int
///
///   fn (n int) -> cls (int) -> int
///     → outer: params=[n], shape=Fn,  returnType = inner
///     → inner: params=[],  shape=Cls, returnType = int
///
///   fn (a int) fn (b int) -> int      (adjacent form — desugars to the
///     second example above; each group is still explicitly marked)
///
///   fn (a int)(b int) -> int          (MALFORMED — `(b int)` has no marker;
///     the parser rejects this with a "missing 'fn' or 'cls' before
///     parameter group" diagnostic, per the plan's diagnostics section)
///
/// `fn`-marked stages lower to bare `llvm::Function*` values; `cls`-marked
/// stages lower to `{func, env}` fat pointers with a refcounted environment.
/// The shape is static, so CodeGen dispatches on the type, never on a runtime
/// tag.
///
/// @field params        The parameters for this group (raw pointers to ParamAST)
/// @field returnType    Return type — a plain TypeAST or another FuncTypeAST
/// @field shape         The runtime shape of this stage (Fn or Cls)
struct FuncTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FuncType;

    ArenaSpan<ParamAST*> params;      // parameters for this group
    TypeAST* returnType = nullptr;     // return types (may contain FuncTypeAST)
    FuncShape shape = FuncShape::Fn;   // runtime shape of this stage

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

    // ─── Shape Predicates ───────────────────────────────────────────────
    bool isFn()  const { return shape == FuncShape::Fn;  }
    bool isCls() const { return shape == FuncShape::Cls; }
};








