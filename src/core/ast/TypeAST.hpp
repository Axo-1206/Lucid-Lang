/**
 * @file TypeAST.hpp
 *
 * @responsibility The syntactic representation of types: primitives,
 *                 named types, arrays, row references, and function types.
 *
 * @hierarchy BaseAST → TypeAST → [Concrete Type Nodes]
 *
 * ─── Design: five kinds of type ───────────────────────────────────────────
 * A type in Lucid is exactly one of:
 *
 *   - a primitive             (`int`, `float`, `string`, `bool`, `char`, ...)
 *   - a named type            (`Person`, `SpriteRef`, `Direction`)
 *   - an array                (`[T]`, `[N]T`)
 *   - a row reference         (`&T`)
 *   - a function type         (`(T, U) -> R`)
 *
 * There is no nullable type, no fallible type, no value reference. A row
 * reference (`&T`) is inherently nilable; `nil` is an ordinary value of
 * that type (grammar §5.2). Primitives are never nilable. Tables are
 * global and reference-typed; a bare `Person` is the sheet itself.
 *
 * ─── Design: no `fn`/`cls` marker ─────────────────────────────────────────
 * Every function value is a bare code pointer (grammar §4.2.5). A
 * function type names the signature; the representation is uniform.
 *
 * ─── Design: primitive names are keywords, not identifiers ────────────────
 * The primitive type names are recognized directly by the lexer (see
 * Tokens.hpp). The parser produces a `PrimitiveTypeAST` with a
 * `PrimitiveKind` tag; sized aliases (`int`, `int32`) fold into the same
 * kind. There is exactly one internal representation per primitive type,
 * regardless of which spelling the source used.
 */

#pragma once

#include "BaseAST.hpp"

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveKind
// ─────────────────────────────────────────────────────────────────────────────
//
// The internal tag for a primitive type. Aliases in the source — `int`
// vs. `int32`, `long` vs. `int64` — fold to the same kind, so downstream
// code has one representation per type to handle.
//
// The grammar names the canonical forms and their aliases in §2.2. The
// aliases are:
//
//     int    = int32
//     long   = int64
//     uint   = uint32
//     ulong  = uint64
//     float  = float32
//     double = float64

enum class PrimitiveKind : uint8_t {
    Bool,
    Char,
    String,
    Unit,

    Int8,
    Int16,
    Int32,
    Int64,

    Uint8,
    Uint16,
    Uint32,
    Uint64,

    Float32,
    Float64,
};

// ─── PrimitiveKind predicates ─────────────────────────────────────────────

/// @brief The bit width of a primitive numeric kind. Returns 0 for
///        `bool`, `char`, `string`, and `unit`.
inline size_t primitiveBitWidth(PrimitiveKind kind) noexcept {
    switch (kind) {
        case PrimitiveKind::Int8:
        case PrimitiveKind::Uint8:
            return 8;
        case PrimitiveKind::Int16:
        case PrimitiveKind::Uint16:
            return 16;
        case PrimitiveKind::Int32:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Float32:
            return 32;
        case PrimitiveKind::Int64:
        case PrimitiveKind::Uint64:
        case PrimitiveKind::Float64:
            return 64;
        case PrimitiveKind::Bool:
        case PrimitiveKind::Char:
        case PrimitiveKind::String:
        case PrimitiveKind::Unit:
            return 0;
    }
    return 0;
}

inline bool isSignedIntegerKind(PrimitiveKind kind) noexcept {
    switch (kind) {
        case PrimitiveKind::Int8:
        case PrimitiveKind::Int16:
        case PrimitiveKind::Int32:
        case PrimitiveKind::Int64:
            return true;
        default:
            return false;
    }
}

inline bool isUnsignedIntegerKind(PrimitiveKind kind) noexcept {
    switch (kind) {
        case PrimitiveKind::Uint8:
        case PrimitiveKind::Uint16:
        case PrimitiveKind::Uint32:
        case PrimitiveKind::Uint64:
            return true;
        default:
            return false;
    }
}

inline bool isIntegerKind(PrimitiveKind kind) noexcept {
    return isSignedIntegerKind(kind) || isUnsignedIntegerKind(kind);
}

inline bool isFloatKind(PrimitiveKind kind) noexcept {
    return kind == PrimitiveKind::Float32 || kind == PrimitiveKind::Float64;
}

inline bool isNumericKind(PrimitiveKind kind) noexcept {
    return isIntegerKind(kind) || isFloatKind(kind);
}

// ─────────────────────────────────────────────────────────────────────────────
// ArrayKind
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The two array shapes.
///
/// - `Dynamic` — `[T]`. Grows and shrinks via `.ADD`/`.REMOVE`. Owns its
///   backing buffer.
/// - `Fixed`   — `[N]T`. Compile-time length. Inline storage.
///
/// A slice (`[_]T`) existed in the old grammar; it is removed. A
/// non-owning view over an array is expressed by passing the array
/// itself, and the language does not distinguish view from owner at the
/// type level.
enum class ArrayKind : uint8_t {
    Dynamic,  // [T]
    Fixed,    // [N]T
};

// ─────────────────────────────────────────────────────────────────────────────
// PrimitiveTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A primitive type: `int`, `float`, `string`, `bool`, `char`, ...
///
/// The parser produces this node when it sees one of the primitive type
/// keywords. Sized aliases fold into the canonical kind: `int` and
/// `int32` both produce `PrimitiveKind::Int32`.
///
/// @example
///   let x: int    = 5      → PrimitiveKind::Int32
///   let s: string = "hi"   → PrimitiveKind::String
///   let b: bool   = true   → PrimitiveKind::Bool
struct PrimitiveTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::PrimitiveType;

    PrimitiveKind primitiveKind;

    explicit PrimitiveTypeAST(PrimitiveKind k)
        : TypeAST(ASTKind::PrimitiveType), primitiveKind(k) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// NamedTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A user-declared type referenced by name.
///
/// A `NamedTypeAST` names a table (the sheet itself) or a host type.
/// There are no generic arguments in the new grammar; a name is a name.
///
/// The parser produces this node for any identifier that appears in a
/// type position. Sema resolves the name against the type namespace and
/// writes the resolved declaration to `resolvedDecl`.
///
/// @example
///   let p: Person        → name = "Person"
///   let s: SpriteRef     → name = "SpriteRef"
///   let d: Direction     → name = "Direction"
///
/// Resolution targets:
///   - `TableDeclAST` — a table (the sheet itself).
///   - A host-backed table declared with `TABLE X = host("name")`.
///   - A module-qualified name that resolves through the module's exports.
struct NamedTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::NamedType;

    // ─── Parser Fields (immutable) ──────────────────────────────────────
    InternedString name;

    /// The module qualifier, if the type was written `mod.Type`. An
    /// invalid InternedString (id == 0) if the type was unqualified.
    ///
    /// A module-qualified type name refers to a table exported by the
    /// named module. Sema resolves the module by the qualifier and the
    /// table by the name.
    InternedString qualifier;

    // ─── Semantic Fields (set by Sema) ──────────────────────────────────
    /// The resolved declaration for this named type.
    TypeDeclAST* resolvedDecl = nullptr;

    explicit NamedTypeAST(InternedString n)
        : TypeAST(ASTKind::NamedType), name(n) {}

    bool isQualified() const { return qualifier.isValid(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// ArrayTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief An array type: `[T]` (dynamic) or `[N]T` (fixed).
///
/// The element type is stored as a `TypeAST*`; the array kind determines
/// whether `fixedSize` is meaningful.
///
/// Array literals (`[1, 2, 3]`) have their element type inferred from
/// context; the parser produces the literal without a `type`, and Sema
/// fills it in. The array *type* is produced by the type parser when the
/// source writes `[T]` or `[N]T`.
///
/// @example
///   [T]      → kind = Dynamic, element = T
///   [4]int   → kind = Fixed, size = 4, element = PrimitiveTypeAST(Int32)
struct ArrayTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::ArrayType;

    ArrayKind arrayKind;
    uint64_t  fixedSize;   // meaningful only when arrayKind == Fixed
    TypeAST*  element;

    ArrayTypeAST(ArrayKind k, uint64_t sz, TypeAST* elem)
        : TypeAST(ASTKind::ArrayType)
        , arrayKind(k)
        , fixedSize(sz)
        , element(elem) {}

    bool isDynamic() const { return arrayKind == ArrayKind::Dynamic; }
    bool isFixed()   const { return arrayKind == ArrayKind::Fixed; }
};

// ─────────────────────────────────────────────────────────────────────────────
// RowRefTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A row reference: `&T`, where `T` is a table.
///
/// A `&Person` refers to one row of the `Person` sheet. The reference is
/// inherently nilable: `nil` is a valid value of any `&T` type, produced
/// by a lookup that found nothing or by a cell whose referenced row was
/// removed. The `??` operator coalesces nil to a fallback.
///
/// `&` is only valid on a table type. There is no `&int`, no `&string`;
/// primitives are always copied (grammar §5.1.1). If the source writes
/// `&int`, the parser produces this node with a non-table inner type,
/// and Sema reports "row reference requires a table type".
///
/// The name is `RowRefTypeAST`, not `RefTypeAST`, because the new
/// grammar has exactly one reference kind and its referent is always a
/// table row. The old grammar's `RefTypeAST` (a value reference) is
/// gone.
///
/// @example
///   &Person     → inner = NamedTypeAST("Person")
///   &Direction  → inner = NamedTypeAST("Direction")
struct RowRefTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::RowRefType;

    TypeAST* inner;

    explicit RowRefTypeAST(TypeAST* t)
        : TypeAST(ASTKind::RowRefType), inner(t) {}
};

// ─────────────────────────────────────────────────────────────────────────────
// FunctionTypeAST
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A function type: `(T, U) -> R`.
///
/// A function type names the signature of a callable value. Every
/// function value in Lucid is a bare code pointer with no captures and
/// no environment; a function type therefore describes only the
/// signature, not a runtime representation.
///
/// A function value is produced by:
///   - a top-level `FN` declaration, referred to by name;
///   - a lambda literal (`(p) -> p.age < 18`).
///
/// Both forms produce a value of the same function type, and the
/// compiler lowers both to a compile-time code address. There is no
/// difference between the two at the type level.
///
/// Parameters are unnamed: a function type's parameter list is a list of
/// types, not a list of `name: type` pairs. The grammar's `function_type`
/// production is `'(' [ type { ',' type } ] ')' '->' type`. The
/// parameters are therefore stored as a span of `TypeAST*`, not
/// `ParamAST*`.
///
/// @example
///   () -> unit                       → params = {},  returnType = unit
///   (&Person) -> bool                → params = [&Person], returnType = bool
///   (int, string) -> float           → params = [int, string], returnType = float
struct FunctionTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::FunctionType;

    /// The parameter types, in order. May be empty.
    ArenaSpan<TypeAST*> params;

    /// The return type. Never null.
    TypeAST* returnType = nullptr;

    FunctionTypeAST() : TypeAST(ASTKind::FunctionType) {}
};