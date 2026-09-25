/// @file core/ast/ResourceKind.cpp
/// @brief Implementation of the resource-kind classifier.
///
/// ─── Why This Is a Separate Translation Unit ──────────────────────────────
/// The classifier downcasts a `TypeAST*` to its concrete subclass
/// (`FuncTypeAST`, `PrimitiveTypeAST`, ...). Those subclass definitions
/// live in `TypeAST.hpp`, which is included here but not by
/// `ResourceKind.hpp`. That separation is deliberate: `ResourceKind.hpp`
/// only needs the forward declaration of `TypeAST` for its function
/// signature, and the enum's callers shouldn't have to include the full
/// AST to use it.
///
/// ─── The Single Implementation ────────────────────────────────────────────
/// Sema and CodeGen both call `classifyResourceKind` from this file.
/// There is one function and one definition; the two sides cannot drift.

#include "ResourceKind.hpp"
#include "TypeAST.hpp"
#include "DeclAST.hpp"

ResourceKind classifyResourceKind(TypeAST* type) {
    if (!type) return ResourceKind::None;

    // ─── Function-typed bindings ──────────────────────────────────────────
    // Every function value is a fat pointer `{ code, env }`. The
    // environment is refcounted when the function captures, and null
    // otherwise. The classifier cannot distinguish the two from the type
    // alone — whether a given value captures is a runtime fact about the
    // value, not a property of its type — so the classification is
    // conservative: every function value is Refcounted, and the
    // ownership layer handles the null-environment case by checking
    // before retaining or releasing.
    if (type->isa<FuncTypeAST>()) {
        return ResourceKind::Refcounted;
    }

    // ─── Strings ──────────────────────────────────────────────────────────
    // A string is an owned buffer: it owns its bytes. Its copy is a deep
    // copy; its drop frees the buffer unless the buffer is empty.
    if (type->isa<PrimitiveTypeAST>()) {
        return type->as<PrimitiveTypeAST>()->primitiveKind
                == PrimitiveKind::String
            ? ResourceKind::OwnedBuffer
            : ResourceKind::None;
    }

    // ─── Dynamic arrays ───────────────────────────────────────────────────
    // A `[*]T` is a heap-owned buffer. A `[_]T` (slice) is a borrowed view
    // and owns nothing; a `[N]T` (fixed array) is inline storage whose
    // resource-ness depends on its element type, which is the Phase 4
    // `Aggregate` case.
    if (type->isa<ArrayTypeAST>()) {
        return type->as<ArrayTypeAST>()->isDynamic()
            ? ResourceKind::OwnedBuffer
            : ResourceKind::None;
    }

    // ─── Host-backed named types ──────────────────────────────────────────
    // A NamedTypeAST whose declaration is a HostTypeDeclAST carrying a
    // non-None RecognizedHostKind is one of the language's runtime-backed
    // types. Today, the only such kind is Deferred.
    //
    // A NamedTypeAST whose declaration is any other type — a user struct,
    // an enum, an alias, an ordinary #host type — is None: the compiler
    // does not track its resource behavior; the user's registered
    // operations do.
    if (type->isa<NamedTypeAST>()) {
        const auto* named = type->as<NamedTypeAST>();
        if (named->resolvedDecl && named->resolvedDecl->isa<HostTypeDeclAST>()) {
            const auto* host = named->resolvedDecl->as<HostTypeDeclAST>();
            switch (host->recognizedKind) {
                case RecognizedHostKind::Deferred:
                    return ResourceKind::Handle;
                case RecognizedHostKind::None:
                    return ResourceKind::None;
            }
        }
        return ResourceKind::None;
    }

    // ─── Aggregate (Phase 4) ──────────────────────────────────────────────
    // A struct, tuple, T?, T!, or fixed array that contains at least one
    // resource. Sema's job is only to answer "yes, this owns something"
    // or "no, it does not." CodeGen derives the per-field copy and drop
    // glue by walking the type.
    //
    // Phase 4 implements the walk:
    //   - NullableTypeAST / FallibleTypeAST / CombinedTypeAST: recurse
    //     into inner.
    //   - NamedTypeAST pointing at a StructDeclAST: recurse into fields.
    //   - ArrayTypeAST of Fixed kind with a resource element: recurse.
    //
    // Until then, `None` is correct: the resource kinds that would make a
    // type an aggregate (Refcounted, OwnedBuffer) are handled above, and
    // Handle is linear and rejected from aggregates by Sema.
    return ResourceKind::None;
}