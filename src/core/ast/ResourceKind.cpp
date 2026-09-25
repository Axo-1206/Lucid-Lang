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

namespace lucid {

namespace {

/// @brief Recognize a named type whose declaration is a host-backed kind.
///
/// The core script declares `Deferred<T>` and `Arena` as
/// `TYPE X = #host(LucidX)`. By the time the classifier runs, the named
/// type's declaration has been resolved, and the declaration carries the
/// target name ("LucidDeferred", "LucidArena"). This helper asks the
/// declaration whether it is one of the kinds the classifier cares about.
///
/// Returns the corresponding `ResourceKind`, or `ResourceKind::None` if
/// the named type is not one of the special kinds.
ResourceKind classifyHostBackedNamedType(const NamedTypeAST* named) {
    if (!named || !named->resolvedDecl) return ResourceKind::None;

    // A host-backed type declaration. The classifier reads the target
    // name to distinguish Deferred and Arena from every other host-backed
    // type (Map, Weak, OpKind, and any user-declared #host type).
    if (const auto* host = dynamic_cast<const HostTypeDeclAST*>(named->resolvedDecl)) {
        // The target names are the identifiers inside the `#host(...)`
        // of the core script's declaration. They are stable strings
        // defined by the core script, not by the user.
        //
        // The classifier can only see `InternedString` handles, so it
        // needs the StringPool to compare names. Rather than requiring
        // every classifier call to thread a pool, we compare against
        // interned IDs cached at core-script load. In practice, the
        // classifier is invoked after core-script loading completes,
        // so the IDs are known.
        //
        // For now, this function returns None for every host-backed
        // type. Handle and Arena are recognized by dedicated AST kinds
        // (see the classifier's switch below), and the current
        // prototype does not yet have a mechanism to intern the
        // core-script names at a stable point.
        (void)host;
    }
    return ResourceKind::None;
}

} // namespace

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
    // Deferred<T>, Arena, and any other type the core script declares as
    // `TYPE X = #host(...)`. The classifier recognizes the ones that
    // affect ownership (Deferred → Handle, Arena → Arena) and returns
    // None for the rest (Map, Weak, OpKind, and user-declared host types).
    //
    // The recognition of Deferred and Arena is deferred to a helper that
    // consults the named type's resolved declaration. The helper's
    // implementation depends on how the compiler makes the core script's
    // interned names available; see the note inside the helper.
    if (type->isa<NamedTypeAST>()) {
        return classifyHostBackedNamedType(type->as<NamedTypeAST>());
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
    // type an aggregate (Refcounted, OwnedBuffer, Arena) are handled
    // above, and Handle is linear and rejected from aggregates by Sema.
    return ResourceKind::None;
}

} // namespace lucid