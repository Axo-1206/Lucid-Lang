/// @file core/ast/ResourceKind.cpp
/// @brief Implementation of the resource-kind classifier.

#include "ResourceKind.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"

ResourceKind classifyResourceKind(TypeAST* type) {
    if (!type) return ResourceKind::None;

    // ─── Primitives ───────────────────────────────────────────────────────
    // Strings own their bytes. Every other primitive is inline and owns
    // nothing.
    if (type->isa<PrimitiveTypeAST>()) {
        return type->as<PrimitiveTypeAST>()->primitiveKind
                    == PrimitiveKind::String
            ? ResourceKind::OwnedBuffer
            : ResourceKind::None;
    }

    // ─── Function values ──────────────────────────────────────────────────
    // A function value is a compile-time code address. No captures, no
    // environment, no allocation. It owns nothing.
    if (type->isa<FunctionTypeAST>()) {
        return ResourceKind::None;
    }

    // ─── Row references ───────────────────────────────────────────────────
    // A `&T` is a pointer to a row in some table. Copying the reference
    // copies the pointer; the row is owned by the table, not by the
    // reference. It owns nothing.
    if (type->isa<RowRefTypeAST>()) {
        return ResourceKind::None;
    }

    // ─── Arrays ───────────────────────────────────────────────────────────
    // A dynamic array `[T]` owns its backing buffer; a copy deep-copies
    // it and a drop frees it. A fixed array `[N]T` owns its elements
    // inline: it is Aggregate if any element is a resource, otherwise
    // None.
    if (type->isa<ArrayTypeAST>()) {
        auto* arr = type->as<ArrayTypeAST>();
        if (arr->isDynamic()) return ResourceKind::OwnedBuffer;
        // Fixed array. Recurse into the element type.
        return isResourceKind(classifyResourceKind(arr->element))
            ? ResourceKind::Aggregate
            : ResourceKind::None;
    }

    // ─── Named types ──────────────────────────────────────────────────────
    // A name resolves to either a table or a host-backed type.
    //
    //   - A columned table is a reference; the value owns nothing.
    //   - A host-backed table is an opaque host handle; the host manages
    //     its lifetime, and the convention is that host handles are
    //     refcounted.
    //
    // A named type whose `resolvedDecl` has not been set is a Sema
    // error; the classifier returns None to be safe.
    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        if (!named->resolvedDecl) return ResourceKind::None;
        if (named->resolvedDecl->isa<TableDeclAST>()) {
            auto* table = named->resolvedDecl->as<TableDeclAST>();
            return table->isHostBacked
                ? ResourceKind::Refcounted
                : ResourceKind::None;
        }
        return ResourceKind::None;
    }

    // ─── Everything else ──────────────────────────────────────────────────
    // Unknown / error-recovery nodes, and any future type node that does
    // not have a resource semantics, classify as None.
    return ResourceKind::None;
}