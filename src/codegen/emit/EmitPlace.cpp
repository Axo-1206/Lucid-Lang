/// @file codegen/emit/EmitPlace.cpp
/// @brief Place construction and the single write path.
///
/// ─── Why This File Is Separate ────────────────────────────────────────────
/// The write path (`Emitter::store`) and the place-construction helpers
/// are the pieces the redesign is most concerned with. Putting them in
/// their own file makes them easy to find and review, and it keeps the
/// expression emitters (`expr/Emit*.cpp`) focused on producing values.
///
/// ─── The Ownership Invariant ──────────────────────────────────────────────
/// Every write to a place goes through `Emitter::store`. There is no other
/// way to write storage in the emitter. This is enforced by convention:
/// `builder.CreateStore` is only called from `store` (and from the two
/// places in `Ownership` that must write, which are not the emitter's
/// concern). Every other emitter that wants to write a value builds a
/// `Place` and calls `store`.
///
/// The invariant is what makes the ownership rules correct. If a write
/// site bypasses `store`, it skips `intoOwned` (leaking a claim) and
/// skips dropping the old value (leaking the old claim). Both are bugs
/// the single-write-path design prevents.
///
/// ─── An "Anonymous Place" ─────────────────────────────────────────────────
/// The `decl` parameter to `store` identifies the binding the place
/// belongs to. When `decl` is null, the place is **anonymous** — a
/// temporary slot, a closure env field, a struct-literal field being
/// built, a slice's backing storage. Two consequences:
///
///   - The old-value drop is skipped. There's no `alive` tracker for an
///     anonymous place, so `store` can't tell whether the slot holds a
///     valid value to drop. The caller is responsible for making the
///     first write the only write, or for having initialized the slot
///     explicitly.
///
///   - The alive-tracking update is skipped. Anonymous places don't
///     participate in scope cleanup; whoever allocated the slot is
///     responsible for releasing it (or for knowing it doesn't own
///     anything).
///
/// ─── What a Place Is Not ──────────────────────────────────────────────────
/// A `Place` is not an `ExprAST*`. It's the *result* of lowering an
/// l-value expression: a pointer plus the AST type of what the pointer
/// points at. Once you have a `Place`, you don't need the AST node
/// anymore — `store(place, val)` needs only the pointer and the type.
///
/// ─── Places of Different Kinds ────────────────────────────────────────────
/// Three kinds of l-value expressions produce places:
///
///   - Identifier: the binding's storage pointer. For a local, this is
///     an `alloca`. For a parameter, the argument alloca. For a captured
///     variable inside a closure, the env-loaded value or the spill slot.
///     For a module-level binding, `emitModuleAccess` produces a GEP into
///     the module instance — but `emitModuleAccess` is an r-value emitter
///     in `expr/EmitAccess.cpp`, and the l-value form for module access
///     goes through `emitPlace`'s `ModuleAccessExpr` case, which the
///     current file doesn't have yet. See "Future Extensions" below.
///
///   - Field access: a GEP into a struct. The object must be an l-value
///     (or a pointer), because you can't assign to a field of a
///     temporary.
///
///   - Index: a GEP into an array's backing storage. For a fixed array,
///     the place is a pointer into the array value's storage (the
///     alloca or the aggregate's GEP). For a slice, the place is a
///     pointer into the backing buffer the slice views — not a
///     sub-place of the slice value's storage.
///
/// ─── Future Extensions ────────────────────────────────────────────────────
/// Three cases the current file doesn't handle:
///
///   - `ModuleAccessExprAST` l-values (`mod:x = value`): a GEP into the
///     module instance global. The r-value path in `EmitAccess.cpp` has
///     the code; the l-value path can mirror it with `store` instead of
///     `load`.
///
///   - Deref l-values (`*ptr = value`): the language's "sealed conduit"
///     rules forbid direct dereference, so this case may never need to
///     exist. If it does, it's a direct store through the pointer.
///
///   - The captured-variable l-value inside a closure body: the closure
///     body's `FunctionState` binds the capture to a spill alloca (for
///     by-value captures) or directly to the env-loaded pointer (for
///     by-reference captures). Both shapes are already valid `Place`
///     pointers, so `emitIdentifierPlace` handles them without special
///     casing. The comment in `emitIdentifierPlace` notes this.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// store — the single write path
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── The Four Steps ───────────────────────────────────────────────────────
//   1. Acquire a fresh claim for the incoming value (`intoOwned`).
//   2. If the place is a tracked binding and it's alive, load and drop
//      the old value.
//   3. Store the new value.
//   4. If the place is a tracked binding and it wasn't alive, mark it
//      alive.
//
// ─── Why `intoOwned` Runs First ───────────────────────────────────────────
// The value passed to `store` may be `Borrowed` (a load from a binding)
// or `Owned` (a fresh value). `store` needs the value to carry a claim it
// can transfer to the place, so it calls `intoOwned` before writing. The
// result of `intoOwned` is what gets stored.
//
// ─── Why the Old-Value Drop Is Conditional ────────────────────────────────
// A freshly allocated binding holds undefined data. Loading and dropping
// it would be a use-after-free (the drop would call `free` on an
// arbitrary pointer). So the drop is guarded by the binding's alive
// state: a binding that isn't in the `alive` set holds no claim, and its
// old value must not be dropped.
//
// ─── Why Marking Alive Is Conditional ─────────────────────────────────────
// `markAlive` records the binding in the scope's tracker, which is what
// scope-exit cleanup iterates. If the binding was already alive (a
// reassignment), it's already tracked; re-adding is harmless (the set
// ignores duplicates) but redundant. If the binding was not alive (its
// initial store), this is the moment it becomes the scope's
// responsibility. Marking it here — instead of at declaration time —
// is what makes the initial store's drop-skip correct: the drop only
// happens after the first store, because `store` is what marks alive.

void Emitter::store(Place place, Val val, ValueDeclAST* decl) {
    assert(place.isValid() && "store() called with an invalid place");
    assert(val.isValid() && "store() called with an invalid value");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Step 1: Acquire a fresh claim for the incoming value ─────────────
    // If `val` is `Owned`, this is a no-op. If `val` is `Borrowed`, this
    // copies: retain for `Refcounted`, deep-copy for `OwnedBuffer`,
    // per-field copy for `Aggregate`, no-op for scalars.
    //
    // After this step, `owned` carries a claim the place will take over.
    // The caller's original `val` is not used after this point.
    Val owned = program.ownership().intoOwned(val, b);
    if (!owned.isValid()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, place.ty ? place.ty->loc : SourceLocation{},
            "failed to acquire a claim for the value being stored");
        return;
    }

    // ─── Step 2: Drop the old value if the binding is alive ───────────────
    // `isAlive` is only meaningful when `decl` is non-null. For an
    // anonymous place, the store trusts the caller that the slot either
    // doesn't hold a claim (fresh alloca) or that the caller has already
    // handled the old value.
    const bool isAlive = (decl != nullptr) && func().isAlive(decl);

    if (isAlive) {
        // The place's type determines how to load the old value. Using
        // `place.ty` (rather than `owned.v->getType()`) is important: the
        // stored value might be a coerced or transformed version of the
        // place's declared type, and the *place* is authoritative for
        // what the storage holds.
        llvm::Type* oldTy = place.ty
            ? program.types().get(place.ty)
            : owned.v->getType();
        if (oldTy) {
            llvm::Value* old = b.CreateLoad(
                oldTy, place.ptr, "store.old");
            program.ownership().drop(place.ty, old, b);
        }
    }

    // ─── Step 3: Store the new value ──────────────────────────────────────
    // With opaque pointers, no cast is needed. The `owned.v` was
    // produced by `intoOwned` from a value of the place's type, so the
    // LLVM types match.
    b.CreateStore(owned.v, place.ptr);

    // ─── Step 4: Mark the binding alive if it wasn't ──────────────────────
    // The store just acquired the place's initial claim; the binding is
    // now the scope's responsibility.
    if (decl && !isAlive) {
        func().markAlive(decl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// loadPlace — read a value from a place
// ─────────────────────────────────────────────────────────────────────────────
//
// The returned `Val` is `Borrowed`: the place still holds the claim, and
// the loaded value is an alias. Anyone who wants to store the loaded
// value must call `intoOwned` first (which `store` does).
//
// ─── When to Use This vs. `emitIdentifier` ────────────────────────────────
// `loadPlace` is the generic "load from any Place" helper. It's used by
// `emitAssign` for compound assignment (`x += 1` loads `x`'s old value),
// by the self-assign guard, and by any other site that has a `Place` and
// wants the value.
//
// `emitIdentifier` is the specialized version for identifier expressions;
// it produces the same value but goes through the AST node's resolved
// declaration to find the binding. For a plain `x` in value position,
// `emitIdentifier` is what runs. For `loadPlace(emitPlace(x))`, the
// result is the same value, but the path is different.

Val Emitter::loadPlace(Place place, llvm::IRBuilder<>& b) {
    assert(place.isValid() && "loadPlace() called with an invalid place");

    llvm::Type* llvmTy = program.types().get(place.ty);
    if (!llvmTy) return {};

    llvm::Value* loaded = b.CreateLoad(llvmTy, place.ptr, "load.place");

    return Val{loaded, place.ty, Own::Borrowed};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitPlace — the l-value dispatcher
// ─────────────────────────────────────────────────────────────────────────────
//
// Routes by expression kind to the specific place emitter. Only three
// kinds have places today; everything else returns an invalid `Place`.
//
// ─── Who Calls This ───────────────────────────────────────────────────────
// `emitAssign` (in `expr/EmitWrite.cpp`) calls it on `expr->lhs` to get
// the target of the assignment. `emitFieldAccess` and `emitIndex` (in
// `expr/EmitAccess.cpp`) call it on their `object`/`target` when the
// object is an l-value, so they can GEP into the storage rather than
// loading the whole aggregate and extracting a field.
//
// ─── Why "Only Assignable Expressions Have Places" ────────────────────────
// A `Place` is the l-value form of an expression. Not every expression
// has one: `1 + 2` has no storage, `f()` has no storage (the return value
// is in a register, not a place), `Point { ... }` has no place (it's a
// temporary aggregate). Only expressions that *name* storage have places:
// identifiers (a binding), field accesses on l-values (a sub-slot), and
// index expressions on l-value arrays (an element slot).

Place Emitter::emitPlace(ExprAST* expr) {
    if (!expr) return {};

    switch (expr->kind) {
        case ASTKind::IdentifierExpr:
            return emitIdentifierPlace(expr->as<IdentifierExprAST>());

        case ASTKind::FieldAccessExpr:
            return emitFieldPlace(expr->as<FieldAccessExprAST>());

        case ASTKind::IndexExpr:
            return emitIndexPlace(expr->as<IndexExprAST>());

        // Future extensions (see the file header):
        //
        // case ASTKind::ModuleAccessExpr:
        //     return emitModulePlace(expr->as<ModuleAccessExprAST>());
        //
        // case ASTKind::UnaryExpr:
        //     // Only `*ptr` if dereference l-values ever land.
        //     if (expr->as<UnaryExprAST>()->op == UnaryOp::Deref)
        //         return emitDerefPlace(expr->as<UnaryExprAST>());
        //     return {};

        default:
            // Every other expression kind is a value, not a place. The
            // caller who asked for a place is responsible for checking
            // the AST before calling; returning an invalid place lets
            // the caller's assertion fire with a clear signal.
            return {};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIdentifierPlace — a binding's storage
// ─────────────────────────────────────────────────────────────────────────────
//
// A local binding's storage is an `alloca`. A parameter's storage is the
// argument alloca. A captured variable inside a closure body has its
// storage set by `emitClosureBody`: a spill alloca for a by-value
// capture, or the env-loaded pointer for a by-reference capture. All
// three are pointers, and all three are valid `Place`s.
//
// ─── The `cls`-Shaped FuncDecl Exception ──────────────────────────────────
// A `cls`-shaped `FuncDeclAST` binding is a fat pointer stored by value
// in the value map, not a pointer to storage. It has no `Place`: you
// can't assign into a `cls` binding's fat pointer slot in the current
// language (reassignment of a `cls` declaration goes through the
// declaration's initializer, not through an l-value assignment). The
// emitter returns an invalid `Place` for that case, and the caller's
// assertion catches any attempt to use it.
//
// ─── The `_` Discard Placeholder ──────────────────────────────────────────
// `_` is not a valid l-value. Reaching here with `_` is a Sema bug.

Place Emitter::emitIdentifierPlace(IdentifierExprAST* expr) {
    assert(expr && "emitIdentifierPlace() with null expression");

    // ─── `_` ──────────────────────────────────────────────────────────────
    if (program.pool.lookupView(expr->name) == "_") {
        program.diagnostics.errorAt(
            DiagCode::Sem_UndefinedValue, expr->loc,
            "cannot use '_' as an assignment target");
        return {};
    }

    ValueDeclAST* decl = expr->resolvedDecl;
    if (!decl) {
        program.diagnostics.errorAt(
            DiagCode::Sem_UndefinedValue, expr->loc,
            "identifier '", program.pool.lookup(expr->name),
            "' was not resolved");
        return {};
    }

    // ─── Function bindings have no place ──────────────────────────────────
    // A `fn`-shaped function reference is a global symbol; it's not a
    // storage slot you can assign into. A `cls`-shaped binding is a fat
    // pointer held by value; there's no alloca to point at. Sema
    // rejects assigning to either; the emitter returns an invalid place
    // as a defensive measure.
    if (decl->isa<FuncDeclAST>()) {
        return {};
    }

    // ─── Enum variants have no place ──────────────────────────────────────
    if (decl->isa<EnumVariantAST>()) {
        return {};
    }

    // ─── Look up the binding ──────────────────────────────────────────────
    llvm::Value* binding = func().lookupValue(decl);
    if (!binding) {
        program.diagnostics.errorAt(
            DiagCode::Sem_UndefinedValue, expr->loc,
            "identifier '", program.pool.lookup(expr->name),
            "' has no LLVM binding");
        return {};
    }

    // ─── Sanity: a place must be a pointer ────────────────────────────────
    // A non-pointer binding is an SSA value held by value, which can't
    // be a place. The one such case is the `cls` FuncDecl binding,
    // handled above; anything else reaching here is a bug.
    if (!binding->getType()->isPointerTy()) {
        return {};
    }

    return Place{binding, decl->type};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFieldPlace — a GEP into a struct's field
// ─────────────────────────────────────────────────────────────────────────────
//
// A field access on an l-value struct is itself an l-value. The object
// must be a storage location (a binding, another field access, an index),
// because you can't assign to a field of a temporary.
//
// ─── The Object's Place vs. Value ─────────────────────────────────────────
// The object must have a `Place` for the field to have a `Place`. If
// `emitPlace(expr->object)` returns an invalid place, the field access
// isn't an l-value either; the emitter returns an invalid `Place` and the
// caller (usually `emitAssign`) reports the error.
//
// ─── Struct Resolution ────────────────────────────────────────────────────
// `expr->resolvedDecl` is the `FieldDeclAST`. `expr->ownerType` is the
// struct (or enum) that owns the field. Both are set by Sema; the
// emitter reads them without re-checking the AST's shape.

Place Emitter::emitFieldPlace(FieldAccessExprAST* expr) {
    assert(expr && "emitFieldPlace() with null expression");

    // ─── Enum variant access has no place ─────────────────────────────────
    // `Direction.North` is a constant, not storage.
    if (expr->isEnumAccess) return {};

    // ─── Object place ─────────────────────────────────────────────────────
    Place objPlace = emitPlace(expr->object);
    if (!objPlace.isValid()) return {};

    // ─── Field declaration ────────────────────────────────────────────────
    FieldDeclAST* field = expr->resolvedDecl
        ? (expr->resolvedDecl->isa<FieldDeclAST>()
              ? expr->resolvedDecl->as<FieldDeclAST>()
              : nullptr)
        : nullptr;
    if (!field) {
        program.diagnostics.errorAt(
            DiagCode::Sem_FieldNotFound, expr->loc,
            "field '", program.pool.lookup(expr->fieldName),
            "' has no resolved declaration");
        return {};
    }

    // ─── Owning struct ────────────────────────────────────────────────────
    StructDeclAST* structDecl = expr->ownerType
        ? (expr->ownerType->isa<StructDeclAST>()
              ? expr->ownerType->as<StructDeclAST>()
              : nullptr)
        : nullptr;
    if (!structDecl) {
        program.diagnostics.errorAt(
            DiagCode::Sem_FieldNotFound, expr->loc,
            "field '", program.pool.lookup(expr->fieldName),
            "' has no owning struct");
        return {};
    }

    llvm::StructType* structTy = program.types().structType(structDecl);
    if (!structTy) return {};

    // ─── Field index ──────────────────────────────────────────────────────
    // Sema caches the field's index on the AST; the emitter reads it and
    // falls back to `indexOfField` if the cache is missing.
    size_t fieldIndex = expr->fieldIndex;
    if (fieldIndex == SIZE_MAX) {
        fieldIndex = structDecl->indexOfField(field->name);
        if (fieldIndex == SIZE_MAX) {
            program.diagnostics.errorAt(
                DiagCode::Sem_FieldNotFound, expr->loc,
                "field '", program.pool.lookup(expr->fieldName),
                "' has no index in struct '",
                program.pool.lookup(structDecl->name), "'");
            return {};
        }
        expr->fieldIndex = fieldIndex;
    }

    // ─── GEP to the field ─────────────────────────────────────────────────
    llvm::IRBuilder<>& b = program.builder();
    llvm::Value* fieldPtr = b.CreateStructGEP(
        structTy,
        objPlace.ptr,
        static_cast<unsigned>(fieldIndex),
        "place.field." + program.pool.lookup(field->name));

    return Place{fieldPtr, field->type};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIndexPlace — a GEP into an array's backing storage
// ─────────────────────────────────────────────────────────────────────────────
//
// The place's address depends on the array's kind:
//
//   - Fixed array `[N]T`: the array value's storage is a contiguous block
//     of `N` elements. The place is a GEP to the `i`-th element.
//
//   - Slice `[_]T`: the slice value is a `{ ptr, i64 len, i64 cap }`
//     struct. The backing storage lives elsewhere (whatever the slice
//     views), and the place is a GEP through the slice's data pointer.
//     The place's address is **not** inside the slice value's storage —
//     it's inside the buffer the slice views.
//
//   - Dynamic array `[*]T`: the current lowering is a bare `ptr`. The
//     place is a GEP through the pointer, but with no length check
//     available. Diagnosed.
//
// ─── Bounds ───────────────────────────────────────────────────────────────
// `emitIndexPlace` doesn't emit a bounds check. The assignment emitter
// that calls it is responsible for the check (the same way `emitAssign`
// checks the RHS's range before writing). This is deliberate: emitting
// the bounds check in `emitIndexPlace` would emit it twice for
// `arr[i] = arr[i]` (once for the LHS place, once for the RHS value).
//
// If the assignment path decides it wants a check (because the language
// requires it), it's a `emitSliceBoundsCheck` / `emitFixedArrayBoundsCheck`
// call in `emitAssign`, not here.

Place Emitter::emitIndexPlace(IndexExprAST* expr) {
    assert(expr && "emitIndexPlace() with null expression");

    // ─── Target place ─────────────────────────────────────────────────────
    Place targetPlace = emitPlace(expr->target);
    if (!targetPlace.isValid()) return {};

    // ─── Array type ───────────────────────────────────────────────────────
    ArrayTypeAST* arrayTy = expr->target->resolvedType
        ? (expr->target->resolvedType->isa<ArrayTypeAST>()
              ? expr->target->resolvedType->as<ArrayTypeAST>()
              : nullptr)
        : nullptr;
    if (!arrayTy) {
        program.diagnostics.errorAt(
            DiagCode::Sem_InvalidArrayElement, expr->target->loc,
            "index target is not an array type");
        return {};
    }

    llvm::Type* elemTy = program.types().get(arrayTy->element);
    if (!elemTy) return {};

    // ─── Index ────────────────────────────────────────────────────────────
    Val index = emit(expr->index);
    if (!index.isValid()) return {};

    llvm::Type* i64Ty = llvm::Type::getInt64Ty(program.llvmContext());
    llvm::Value* indexVal = index.v;
    if (indexVal->getType() != i64Ty) {
        indexVal = coerceValueToType(indexVal, i64Ty, program.builder());
        if (!indexVal) return {};
    }

    // ─── Dispatch on array kind ───────────────────────────────────────────
    llvm::IRBuilder<>& b = program.builder();
    llvm::Value* elemPtr = nullptr;

    if (arrayTy->isFixed()) {
        // The target's storage is the array value's storage. GEP to the
        // first element, then GEP by the index.
        llvm::ArrayType* arrTy = llvm::dyn_cast<llvm::ArrayType>(
            program.types().get(arrayTy));
        if (!arrTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "fixed array type has an unexpected LLVM shape");
            return {};
        }
        llvm::Value* dataPtr = b.CreateConstGEP2_32(
            arrTy, targetPlace.ptr, 0, 0, "place.index.data");
        elemPtr = b.CreateGEP(
            elemTy, dataPtr, indexVal, "place.index.ptr");
    } else if (arrayTy->isSlice()) {
        // The slice's storage holds the `{ ptr, i64, i64 }` value. Load
        // the data pointer (field 0), then GEP through it. The place's
        // address is inside the backing buffer, not the slice struct.
        llvm::StructType* sliceTy = llvm::dyn_cast<llvm::StructType>(
            program.types().get(arrayTy));
        if (!sliceTy || sliceTy->getNumElements() != 3) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "slice type has an unexpected LLVM shape");
            return {};
        }
        llvm::Value* sliceVal = b.CreateLoad(
            sliceTy, targetPlace.ptr, "place.index.slice");
        llvm::Value* dataPtr = b.CreateExtractValue(
            sliceVal, 0, "place.index.slice.data");
        elemPtr = b.CreateGEP(
            elemTy, dataPtr, indexVal, "place.index.ptr");
    } else if (arrayTy->isDynamic()) {
        // The current dynamic-array lowering is a bare `ptr`. Its place
        // is a pointer to a pointer; load the pointer, then GEP.
        llvm::Type* ptrTy = llvm::PointerType::get(program.llvmContext(), 0);
        llvm::Value* dataPtr = b.CreateLoad(
            ptrTy, targetPlace.ptr, "place.index.dyn.data");
        elemPtr = b.CreateGEP(
            elemTy, dataPtr, indexVal, "place.index.ptr");
        // No bounds check possible; the caller's responsibility.
    } else {
        return {};
    }

    return Place{elemPtr, arrayTy->element};
}

} // namespace codegen