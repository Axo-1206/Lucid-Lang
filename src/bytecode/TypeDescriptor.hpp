/**
 * @file TypeDescriptor.hpp
 *
 * @responsibility The serializable form of a Lucid type. The artifact
 *                 cannot store TypeAST* pointers; it stores these.
 *
 * ─── Design: a flat tagged union, not a graph ─────────────────────────────
 * A TypeDescriptor is a small, fully-owned value. Nested types (arrays
 * of arrays, function types, row references) are held by value in a
 * vector of component descriptors. There are no cycles: the compiler
 * flattens a recursive type at the point of use, and the language has
 * no recursive types (a table references itself only through &T, which
 * is a single index, not a nested TypeDescriptor).
 *
 * ─── Design: kinds mirror TypeAST's concrete subclasses ───────────────────
 * One Kind per concrete TypeAST subclass. The translation from TypeAST
 * to TypeDescriptor is in BakeConstant.cpp / EmitDecl.cpp and is a
 * direct structural copy.
 */

#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace lucid::bytecode {

// Forward — PrimitiveKind is defined in core/ast/TypeAST.hpp. The
// bytecode layer mirrors it rather than including the AST header, so
// this header has no dependency on the frontend.
enum class PrimitiveKindMirror : uint8_t {
    Bool, Char, String, Void,
    Int8, Int16, Int32, Int64,
    Uint8, Uint16, Uint32, Uint64,
    Float32, Float64,
};

enum class ArrayKindMirror : uint8_t {
    Dynamic, Fixed,
};

struct TypeDescriptor;
using TypeDescriptorList = std::vector<TypeDescriptor>;

/// @brief A serializable type.
struct TypeDescriptor {
    enum class Kind : uint8_t {
        Primitive,     ///< PrimitiveKindMirror
        Named,         ///< a table or host type, by mangled name
        Array,         ///< ArrayKindMirror + element type
        RowRef,        ///< &T; inner is a Named
        Function,      ///< params + return
        Nullable,      ///< inner is the non-nil type
        Unknown,       ///< error-recovery placeholder
    };

    Kind kind = Kind::Unknown;

    // Primitive
    PrimitiveKindMirror primitive = PrimitiveKindMirror::Void;

    // Named
    std::string namedMangled;   ///< mangled name of the table/host type

    // Array
    ArrayKindMirror  arrayKind = ArrayKindMirror::Dynamic;
    uint64_t         fixedSize = 0;

    // Array / RowRef / Nullable — the single component type
    // Function — params[0..n-1], and `component` holds the return type
    // Held indirectly so a recursive structure is a heap allocation,
    // not an infinite-size struct. In practice a Lucid type is finite.
    std::shared_ptr<TypeDescriptor>      component;
    std::vector<std::shared_ptr<TypeDescriptor>> params;  ///< Function only

    // ─── Factories ──────────────────────────────────────────────────────
    static TypeDescriptor makePrimitive(PrimitiveKindMirror p);
    static TypeDescriptor makeNamed(std::string mangled);
    static TypeDescriptor makeArray(ArrayKindMirror k, uint64_t size,
                                    TypeDescriptor element);
    static TypeDescriptor makeRowRef(TypeDescriptor inner);
    static TypeDescriptor makeFunction(TypeDescriptorList params,
                                       TypeDescriptor returnType);
    static TypeDescriptor makeNullable(TypeDescriptor inner);

    bool isPrimitive() const noexcept { return kind == Kind::Primitive; }
    bool isNamed()     const noexcept { return kind == Kind::Named; }
    bool isArray()     const noexcept { return kind == Kind::Array; }
    bool isRowRef()    const noexcept { return kind == Kind::RowRef; }
    bool isFunction()  const noexcept { return kind == Kind::Function; }
    bool isNullable()  const noexcept { return kind == Kind::Nullable; }
};

/// @brief A serializable function signature. Matches FunctionTypeAST.
struct FunctionSignature {
    TypeDescriptorList params;
    TypeDescriptor     returnType;
};

} // namespace lucid::bytecode