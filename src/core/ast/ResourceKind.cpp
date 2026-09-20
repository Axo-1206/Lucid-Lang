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
/// Sema and CodeGen both call `classifyResourceKind` from this file. Before
/// the redesign, Sema had its own member function and CodeGen had a
/// duplicate free function; the two drifted. Now there is one function,
/// used by both, and the invariant is enforced by the linker: there is
/// nothing to keep in sync because there is nothing duplicated.

#include "ResourceKind.hpp"
#include "TypeAST.hpp"

namespace codegen {

ResourceKind classifyResourceKind(TypeAST* type) {
    if (!type) return ResourceKind::None;

    // ─── Function-typed bindings ──────────────────────────────────────────
    // Under the `fn`/`cls` design, a function type's shape is part of the
    // type, not inferred from the initializer. `fn` is a bare pointer and
    // owns nothing. `cls` is a fat pointer whose environment is refcounted.
    //
    // This applies uniformly to every binding site: a FuncDeclAST whose
    // declared type is `cls`, a ParamAST whose type is `cls`, a
    // FieldDeclAST whose type is `cls`. The binding owns its value's
    // environment; the value's own caller (or the struct literal that
    // stored it) is responsible for the retain, per the ownership model's
    // Rule 3.
    if (type->isa<FuncTypeAST>()) {
        return type->as<FuncTypeAST>()->isCls()
            ? ResourceKind::Refcounted
            : ResourceKind::None;
    }

    // ─── Strings ──────────────────────────────────────────────────────────
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

    // ─── Arena ────────────────────────────────────────────────────────────
    // Arena is scope-confined. Its backing block is freed by the ownership
    // layer's drop.
    if (type->isa<ArenaTypeAST>()) {
        return ResourceKind::Arena;
    }

    // ─── ArenaDescriptor ──────────────────────────────────────────────────
    // POD struct `{ base, size }`. No owned resources.
    if (type->isa<ArenaDescriptorTypeAST>()) {
        return ResourceKind::None;
    }

    // ─── Handle (linear async/spawn handles) ──────────────────────────────
    // Future<T> and Thread<T> are linear handles. The inner type's
    // resource kind is deliberately not consulted: the handle itself is a
    // single pointer, and the boxed result is freed by the await/join
    // site, not by scope-exit cleanup of the handle binding.
    if (type->isa<FutureTypeAST>() || type->isa<ThreadTypeAST>()) {
        return ResourceKind::Handle;
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

} // namespace codegen