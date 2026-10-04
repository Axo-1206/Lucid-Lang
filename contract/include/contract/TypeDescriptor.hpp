/**
 * @file TypeDescriptor.hpp
 *
 * @responsibility The serializable form of a Lucid type. The artifact
 *                 cannot store TypeAST* pointers; it stores these.
 *
 * ─── Design: no mirrors ───────────────────────────────────────────────────
 * PrimitiveKind and ArrayKind live in core/PrimitiveKind.hpp and
 * core/ArrayKind.hpp. This file includes those headers directly, not a
 * mirrored copy. One definition per concept; no drift.
 *
 * ─── Design: a tree of owned descriptors, not a graph ─────────────────────
 * A TypeDescriptor is a small, fully-owned value. Nested types (arrays
 * of arrays, function types, row references) are held by shared_ptr so
 * a recursive shape is representable without an infinite-size struct.
 * In practice a Lucid type is finite and the recursion depth is small.
 *
 * ─── Design: one Kind per concrete TypeAST subclass ───────────────────────
 * The translation from TypeAST to TypeDescriptor is a structural copy,
 * performed by the compiler when it reads a resolved type. It happens
 * in EmitDecl.cpp / EmitExpr.cpp via a small helper.
 */

#pragma once

#include "core/PrimitiveKind.hpp"
#include "core/ArrayKind.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lucid::contract {

struct TypeDescriptor;

/// @brief A serializable type.
struct TypeDescriptor {
    enum class Kind : uint8_t {
        Primitive,     ///< payload: primitive
        Named,         ///< payload: namedMangled (a table or host type)
        Array,         ///< payload: arrayKind + fixedSize + component
        RowRef,        ///< payload: component (a Named)
        Function,      ///< payload: params + component (the return type)
        Nullable,      ///< payload: component (the non-nil type)
        Unknown,       ///< error-recovery placeholder
    };

    Kind kind = Kind::Unknown;

    // Primitive
    PrimitiveKind primitive = PrimitiveKind::Void;

    // Named
    std::string namedMangled;       ///< mangled name of the table/host type
    bool        isHostType = false; ///< true if the named type is a
                                    ///< host(...)-backed type

    // Array
    ArrayKind arrayKind = ArrayKind::Dynamic;
    uint64_t         fixedSize = 0;

    // Array / RowRef / Nullable — the single component type.
    // Function — the return type. Parameters are in `params`.
    std::shared_ptr<TypeDescriptor> component;
    std::vector<std::shared_ptr<TypeDescriptor>> params;  ///< Function only

    // ─── Factories ──────────────────────────────────────────────────────
    static TypeDescriptor makePrimitive(PrimitiveKind p);
    static TypeDescriptor makeNamed(std::string mangled);
    static TypeDescriptor makeArray(ArrayKind k, uint64_t size,
                                    TypeDescriptor element);
    static TypeDescriptor makeRowRef(TypeDescriptor inner);
    static TypeDescriptor makeFunction(std::vector<TypeDescriptor> params,
                                       TypeDescriptor returnType);
    static TypeDescriptor makeNullable(TypeDescriptor inner);

    bool isPrimitive() const noexcept { return kind == Kind::Primitive; }
    bool isNamed()     const noexcept { return kind == Kind::Named; }
    bool isArray()     const noexcept { return kind == Kind::Array; }
    bool isRowRef()    const noexcept { return kind == Kind::RowRef; }
    bool isFunction()  const noexcept { return kind == Kind::Function; }
    bool isNullable()  const noexcept { return kind == Kind::Nullable; }
};

} // namespace lucid::contract