/// @file bytecode/TypeDescriptor.cpp
/// @brief Factory methods for the serializable type descriptor.

#include "TypeDescriptor.hpp"

#include <utility>

namespace lucid::bytecode {

// ─────────────────────────────────────────────────────────────────────────────
// Factories
// ─────────────────────────────────────────────────────────────────────────────
//
// Each factory produces a TypeDescriptor with exactly one Kind set and
// the fields that Kind uses. The other fields are left at their
// defaults; a consumer that reads a field not belonging to the Kind is
// reading garbage, and the Kind tag is the contract for which fields
// are meaningful.
//
// The factories take components by value. A TypeDescriptor is small
// (one enum, one primitive kind, one string, one array kind, one
// size, one shared_ptr, one vector) and the components are moved into
// the result. The shared_ptr indirection is what makes a recursive
// shape representable without an infinite-size struct.

TypeDescriptor TypeDescriptor::makePrimitive(PrimitiveKind p) {
    TypeDescriptor d;
    d.kind      = Kind::Primitive;
    d.primitive = p;
    return d;
}

TypeDescriptor TypeDescriptor::makeNamed(std::string mangled) {
    TypeDescriptor d;
    d.kind         = Kind::Named;
    d.namedMangled = std::move(mangled);
    return d;
}

TypeDescriptor TypeDescriptor::makeArray(ArrayKind k, uint64_t size,
                                         TypeDescriptor element) {
    TypeDescriptor d;
    d.kind      = Kind::Array;
    d.arrayKind = k;
    d.fixedSize = size;
    d.component = std::make_shared<TypeDescriptor>(std::move(element));
    return d;
}

TypeDescriptor TypeDescriptor::makeRowRef(TypeDescriptor inner) {
    TypeDescriptor d;
    d.kind      = Kind::RowRef;
    d.component = std::make_shared<TypeDescriptor>(std::move(inner));
    return d;
}

TypeDescriptor TypeDescriptor::makeFunction(std::vector<TypeDescriptor> params,
                                            TypeDescriptor returnType) {
    TypeDescriptor d;
    d.kind = Kind::Function;
    d.params.reserve(params.size());
    for (auto& p : params) {
        d.params.push_back(std::make_shared<TypeDescriptor>(std::move(p)));
    }
    d.component = std::make_shared<TypeDescriptor>(std::move(returnType));
    return d;
}

TypeDescriptor TypeDescriptor::makeNullable(TypeDescriptor inner) {
    TypeDescriptor d;
    d.kind      = Kind::Nullable;
    d.component = std::make_shared<TypeDescriptor>(std::move(inner));
    return d;
}

} // namespace lucid::bytecode