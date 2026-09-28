/// @file core/ast/ResourceKind.cpp
/// @brief Implementation of the resource-kind classifier.

#include "ResourceKind.hpp"

#include "TypeAST.hpp"
#include "DeclAST.hpp"

ResourceKind classifyResourceKind(TypeAST* type) {
    if (type == nullptr) return ResourceKind::None;

    // ─── Nullable type: same kind as inner ────────────────────────────────
    //
    // A `T?` where T owns a resource is still that resource kind. A nil
    // value has nothing to copy or drop, but the *kind* — what the
    // binding would own if non-nil — is the same as the inner type's.
    // Codegen handles the nil case at use sites.
    if (type->isa<NullableTypeAST>()) {
        return classifyResourceKind(type->as<NullableTypeAST>()->inner);
    }

    // ─── Primitives ───────────────────────────────────────────────────────
    // `string` owns its bytes (OwnedBuffer). Every other primitive is
    // inline and owns nothing.
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
    // A `&T` is a reference to a row in some table. Copying the reference
    // copies the row index; the row is owned by the table, not by the
    // reference. It owns nothing.
    if (type->isa<RowRefTypeAST>()) {
        return ResourceKind::None;
    }

    // ─── Arrays ───────────────────────────────────────────────────────────
    // A dynamic array `[T]` owns its backing buffer; a copy deep-copies
    // it and a drop frees it. A fixed array `[N, T]` owns its elements
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
    // A name resolves to a table declaration. In the new grammar, the
    // only TypeDeclAST is TableDeclAST, so the resolution is direct.
    //
    //   - A columned table is a reference; the value owns nothing. The
    //     table owns its rows.
    //   - A host-backed table is an opaque host handle; by convention the
    //     language retains on copy and releases on drop, so the kind is
    //     Refcounted.
    //
    // A named type whose `resolvedDecl` has not been set is a Sema
    // error; the classifier returns None to be safe.
    if (type->isa<NamedTypeAST>()) {
        auto* named = type->as<NamedTypeAST>();
        if (!named->resolvedDecl) return ResourceKind::None;

        // resolvedDecl is a TypeDeclAST*. The only concrete subclass in
        // the new grammar is TableDeclAST. Guard the cast with an isa
        // check so a future TypeDeclAST subclass (if one ever exists)
        // does not silently fall through to a wrong answer.
        if (!named->resolvedDecl->isa<TableDeclAST>()) return ResourceKind::None;

        auto* table = named->resolvedDecl->as<TableDeclAST>();
        return table->isHostBacked
            ? ResourceKind::Refcounted
            : ResourceKind::None;
    }

    // ─── Everything else ──────────────────────────────────────────────────
    // Unknown / error-recovery nodes, and any future type node that does
    // not have a resource semantics, classify as None.
    return ResourceKind::None;
}