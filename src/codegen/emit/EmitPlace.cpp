/// @file codegen/emit/EmitPlace.cpp
/// @brief Place construction and the single write path.
///
/// ─── Why This File Is Separate ────────────────────────────────────────────
/// The write path (`Emitter::store`) and the place-construction helpers
/// are the pieces the redesign is most concerned with. Putting them in
/// their own file makes them easy to find and review, and it keeps the
/// expression emitters (`EmitExpr.cpp`) focused on producing values.
///
/// ─── The Ownership Invariant ──────────────────────────────────────────────
/// Every write to a place goes through `Emitter::store`. There is no other
/// way to write storage in the emitter. This is enforced by convention:
/// `builder.CreateStore` is only called from `store`. Every other emitter
/// that wants to write a value builds a `Place` and calls `store`.
///
/// The invariant is what makes the ownership rules correct. If a write
/// site bypasses `store`, it skips `intoOwned` (leaking a claim) and
/// skips dropping the old value (leaking the old claim). Both are bugs
/// the single-write-path design prevents.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// store — the single write path
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::store(Place place, Val val, ValueDeclAST* decl) {
    assert(place.isValid() && "store() called with an invalid place");
    assert(val.isValid() && "store() called with an invalid value");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Step 1: Acquire a fresh claim for the incoming value ─────────────
    // If `val` is `Borrowed`, this copies (retain for Refcounted, deep-copy
    // for OwnedBuffer, per-field for Aggregate). If `Owned`, it's a no-op.
    //
    // After this step, `owned` carries a claim that the place will take
    // over. The caller's original `val` is not used after this point.
    Val owned = program.ownership().intoOwned(val, b);

    // ─── Step 2: Drop the old value in the place ──────────────────────────
    // Only if the binding is alive — a freshly-allocated binding holds
    // undefined data, and dropping it would be a use-after-free.
    //
    // For anonymous places (decl == nullptr), we assume the place holds
    // a valid value of the place's type. Callers with anonymous places
    // are responsible for ensuring this: a fresh alloca must be
    // explicitly initialized before store is called, or the store must
    // be known to be the first one.
    bool isAlive = (decl != nullptr) && func().isAlive(decl);

    if (isAlive) {
        llvm::Value* old = b.CreateLoad(place.ty ? program.types().get(place.ty)
                                                  : owned.v->getType(),
                                         place.ptr,
                                         "store_old_value");
        program.ownership().drop(place.ty, old, b);
    }

    // ─── Step 3: Store the new value ──────────────────────────────────────
    b.CreateStore(owned.v, place.ptr);

    // ─── Step 4: Mark alive ───────────────────────────────────────────────
    // If this binding wasn't alive before the store, it is now — the
    // store just acquired the place's initial claim.
    if (decl && !isAlive) {
        func().markAlive(decl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// loadPlace — read a value from a place
// ─────────────────────────────────────────────────────────────────────────────

Val Emitter::loadPlace(Place place, llvm::IRBuilder<>& b) {
    assert(place.isValid() && "loadPlace() called with an invalid place");

    llvm::Type* llvmTy = program.types().get(place.ty);
    llvm::Value* loaded = b.CreateLoad(llvmTy, place.ptr, "load_place");

    // The value loaded from a place is `Borrowed`: the place still holds
    // the claim, and the loaded value is an alias. Anyone who wants to
    // store the loaded value must call `intoOwned` first (which `store`
    // does).
    return Val{loaded, place.ty, Own::Borrowed};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitPlace — dispatch by expression kind
// ─────────────────────────────────────────────────────────────────────────────

Place Emitter::emitPlace(ExprAST* expr) {
    if (!expr) return {};

    switch (expr->kind) {
        case ASTKind::IdentifierExpr:
            return emitIdentifierPlace(expr->as<IdentifierExprAST>());

        case ASTKind::FieldAccessExpr:
            return emitFieldPlace(expr->as<FieldAccessExprAST>());

        case ASTKind::IndexExpr:
            return emitIndexPlace(expr->as<IndexExprAST>());

        default:
            // Every other expression kind is a value, not a place. The
            // caller who asked for a place is responsible for the check;
            // we return an invalid place and let the assertion fire.
            return {};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIdentifierPlace — a binding's alloca
// ─────────────────────────────────────────────────────────────────────────────

Place Emitter::emitIdentifierPlace(IdentifierExprAST* expr) {
    assert(expr && "emitIdentifierPlace() with null expression");

    ValueDeclAST* decl = expr->resolvedDecl;
    if (!decl) {
        // Sema should have resolved every identifier. A missing resolved
        // decl here is a Sema bug.
        return {};
    }

    // ─── Local binding ────────────────────────────────────────────────────
    // The binding lives in the `FunctionState`'s value map. For a local
    // variable, it's an alloca; for a parameter, it's the argument alloca;
    // for a captured variable inside a closure, it's the env-loaded value
    // or the spill slot.
    //
    // All of these are pointers to storage — they're all valid places.
    llvm::Value* binding = func().lookupValue(decl);
    if (!binding) {
        return {};
    }

    // Sanity: a place must be a pointer. If the binding is an SSA value
    // (e.g. a closure's fat pointer, which is stored by value, not by
    // pointer), the emitter's value path handles it, not the place path.
    if (!binding->getType()->isPointerTy()) {
        return {};
    }

    return Place{binding, decl->type};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFieldPlace — a GEP into a struct
// ─────────────────────────────────────────────────────────────────────────────

Place Emitter::emitFieldPlace(FieldAccessExprAST* expr) {
    assert(expr && "emitFieldPlace() with null expression");

    // ─── Get the place of the object ──────────────────────────────────────
    // A field access on an l-value struct is itself an l-value. The
    // object must have a place for this to work — you cannot assign to
    // a field of a temporary.
    Place objPlace = emitPlace(expr->object);
    if (!objPlace.isValid()) {
        return {};
    }

    // ─── Get the field declaration and the containing struct ──────────────
    FieldDeclAST* field = expr->resolvedDecl
        ? (expr->resolvedDecl->isa<FieldDeclAST>()
              ? expr->resolvedDecl->as<FieldDeclAST>()
              : nullptr)
        : nullptr;
    if (!field) return {};

    StructDeclAST* structDecl = expr->ownerType
        ? (expr->ownerType->isa<StructDeclAST>()
              ? expr->ownerType->as<StructDeclAST>()
              : nullptr)
        : nullptr;
    if (!structDecl) return {};

    llvm::StructType* structTy = program.types().structType(structDecl);
    if (!structTy) return {};

    // ─── GEP to the field ─────────────────────────────────────────────────
    llvm::IRBuilder<>& b = program.builder();
    llvm::Value* fieldPtr = b.CreateStructGEP(
        structTy, objPlace.ptr, static_cast<unsigned>(field->fieldIndex),
        "field_" + program.pool.lookup(field->name));

    return Place{fieldPtr, field->type};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIndexPlace — a GEP into an array buffer
// ─────────────────────────────────────────────────────────────────────────────

Place Emitter::emitIndexPlace(IndexExprAST* expr) {
    assert(expr && "emitIndexPlace() with null expression");

    // ─── Get the place of the target array ────────────────────────────────
    Place arrPlace = emitPlace(expr->target);
    if (!arrPlace.isValid()) return {};

    // ─── Get the array's element type ─────────────────────────────────────
    ArrayTypeAST* arrayTy = expr->target->resolvedType
        ? (expr->target->resolvedType->isa<ArrayTypeAST>()
              ? expr->target->resolvedType->as<ArrayTypeAST>()
              : nullptr)
        : nullptr;
    if (!arrayTy) return {};

    llvm::Type* elemTy = program.types().get(arrayTy->element);
    if (!elemTy) return {};

    // ─── Emit the index ───────────────────────────────────────────────────
    Val indexVal = emit(expr->index);
    if (!indexVal.isValid()) return {};

    // ─── GEP to the element ───────────────────────────────────────────────
    // Note: the array place points at the array's storage. For a fixed
    // array, that's the storage itself (an `[N x T]` LLVM array); for a
    // dynamic array, that's a pointer to the buffer.
    //
    // The current dynamic-array lowering is a bare `ptr` (see Types.cpp),
    // so the storage holds a `ptr` to the buffer. The GEP must load that
    // pointer first. This is a transitional shape; Task 5's rewrite of
    // the array lowering to a three-field `{ ptr, i64, i64 }` will change
    // the access pattern.
    //
    // For today, the emit assumes the array place points at the buffer
    // directly, matching the current `lowerIndexExpr` behavior.
    llvm::IRBuilder<>& b = program.builder();
    llvm::Value* elemPtr = b.CreateGEP(elemTy, arrPlace.ptr, indexVal.v,
                                        "array_idx_ptr");

    return Place{elemPtr, arrayTy->element};
}

} // namespace codegen