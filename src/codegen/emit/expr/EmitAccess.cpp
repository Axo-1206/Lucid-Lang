/// @file codegen/emit/expr/EmitAccess.cpp
/// @brief Storage-access expression emitters — reads from arrays, slices,
///        struct fields, module state, and arenas.
///
/// ─── What This File Owns ──────────────────────────────────────────────────
/// The expression kinds that read from storage:
///
///   - `emitIndex`        — array / slice element read (`arr[i]`).
///   - `emitSlice`        — slice construction (`arr[lo..hi]`).
///   - `emitFieldAccess`  — struct field read, and enum variant access.
///   - `emitModuleAccess` — cross-module member access (`mod:name`).
///   - `emitArenaAccess`  — Arena method calls (`arena::alloc`, etc.).
///
/// ─── What This File Does NOT Own ──────────────────────────────────────────
///   - The l-value forms of index and field access: `EmitPlace.cpp`
///     (`emitIndexPlace`, `emitFieldPlace`).
///   - Scalar operations and control flow: `expr/EmitScalar.cpp`.
///   - Aggregate construction: `expr/EmitAggregate.cpp`.
///   - Writes: `expr/EmitWrite.cpp`.
///
/// ─── Ownership Tag Conventions ────────────────────────────────────────────
///   - `emitIndex`        → `Borrowed`. The container still holds the
///                          element's claim. A load from an element is an
///                          alias; `store` calls `intoOwned` to acquire a
///                          fresh claim before storing.
///
///   - `emitSlice`        → `Owned`. A slice is a fresh `{ ptr, i64, i64 }`
///                          value that owns no buffer. Its `ResourceKind` is
///                          `None` (slices are not resources), so the tag's
///                          "Owned" is about the *value* being fresh, not
///                          about the slice carrying a claim on the buffer.
///
///   - `emitFieldAccess`  → `Borrowed` for a struct field load; `Owned` for
///                          an enum variant (a `ConstantInt`, which has no
///                          storage and no claim).
///
///   - `emitModuleAccess` → `Borrowed`. The module instance global holds
///                          the field's claim. A function reference is a
///                          global symbol, so it too is `Borrowed`.
///
///   - `emitArenaAccess`  → `Owned`. Arena access allocates a fresh value
///                          (a slice of arena memory, a descriptor, an
///                          integer query result). The allocations have real
///                          claims.
///
/// ─── The Bounds-Check Discipline ──────────────────────────────────────────
/// `emitIndex` and `emitSlice` both perform runtime bounds checks on their
/// indices. The check is:
///
///   1. Compute `inBounds = (0 <= i) && (i < len)`.
///   2. Branch to a panic block on failure, a continue block on success.
///   3. In the panic block, emit a call to `__lucid_panic` with a formatted
///      message and an `unreachable`.
///   4. In the continue block, perform the GEP / load / slice construction.
///
/// The check is emitted for every index unless the index is a compile-time
/// constant that Sema proved in-bounds (a literal index into a fixed-size
/// array, with `0 <= i < N`). In that case the check is skipped: the
/// verifier knows the GEP is in-bounds, and the optimiser removes the
/// branch anyway, so emitting it is pure noise.
///
/// ─── The `??` Interaction ─────────────────────────────────────────────────
/// The old emitter had a null-coalesce-aware mode where a failed index
/// branched to the `??` fallback instead of panicking. The new design
/// keeps that behavior but moves the "am I inside a `??`?" state out of
/// the emitter. `emitNullCoalesce` (in `EmitWrite.cpp`) lowers the fallback
/// by putting the risky expression in a sub-block and branching from a
/// null-coalesce-aware failure. The mechanism is a state flag on the
/// emitter (`program.currentNullCoalesceFallback`), which `emitIndex` and
/// `emitSlice` check before emitting their panic path.
///
/// The flag is set by `emitNullCoalesce` and cleared by the same emitter
/// after the risky expression is lowered. Today the flag doesn't exist on
/// `ProgramState`; a follow-up adds it. Until then, `emitIndex` and
/// `emitSlice` always emit the panic path, and the `??` fallback logic
/// lives in `emitNullCoalesce`'s own block structure (the fallback runs if
/// the risky expression's block terminates without producing a value).
///
/// ─── The Dynamic-Array Transition ─────────────────────────────────────────
/// `Types::arrayType` currently lowers `[*]T` to a bare `ptr`. A bare
/// pointer has no length field, so `emitIndex` and `emitSlice` can't
/// bounds-check a dynamic array. This file handles the fixed-array and
/// slice cases fully, and stubs the dynamic-array case with a diagnostic.
/// When dynamic arrays get the `{ ptr, i64, i64 }` shape, the dynamic
/// branches become the same as the slice branches.

#include "../Emitter.hpp"

#include "codegen/LLVMTypeHelpers.hpp"
#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "runtime/RuntimeError.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <cassert>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitIndex — array / slice element read
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── The Four Shapes of `arr[i]` ──────────────────────────────────────────
// The emitter handles four cases, dispatched on the array's `ArrayKind`:
//
//   - Fixed `[N]T`, constant index in `[0, N)` — no bounds check; the GEP
//     is a `ConstantExpr` (or a direct GEP into the alloca).
//
//   - Fixed `[N]T`, dynamic or out-of-range-unknown index — emit a runtime
//     bounds check `0 <= i < N`.
//
//   - Slice `[_]T` — the value is `{ ptr, i64 len, i64 cap }`. Load field 0
//     for the data pointer, field 1 for the length; bounds-check `i < len`.
//
//   - Dynamic `[*]T` — currently a bare `ptr`. No length to check against.
//     Diagnose.
//
// ─── Reading the Array's Storage ──────────────────────────────────────────
// The target is emitted by `emit(expr->target)`. For a fixed array, the
// value is the array itself (an `[N x T]` aggregate), and the element GEP
// goes through the aggregate's first element. For a slice, the value is the
// `{ ptr, i64, i64 }` struct, and the element GEP goes through field 0.
//
// ─── L-Value vs. R-Value ──────────────────────────────────────────────────
// `emit` returns the loaded value. The l-value form (`emitIndexPlace` in
// `EmitPlace.cpp`) returns the pointer. The two share the type dispatch but
// produce different results; keeping them separate makes each file's job
// clear.

Val Emitter::emitIndex(IndexExprAST* expr) {
    assert(expr && "emitIndex() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Target ───────────────────────────────────────────────────────────
    Val target = emit(expr->target);
    if (!target.isValid()) return {};

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
    if (!elemTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, expr->loc,
            "array element type is unresolvable");
        return {};
    }

    // ─── Index ────────────────────────────────────────────────────────────
    Val index = emit(expr->index);
    if (!index.isValid()) return {};

    // Coerce the index to i64. Every array's index is i64 in the
    // intermediate representation; Sema guarantees the AST type is an
    // integer, and this normalizes the LLVM width.
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(program.llvmContext());
    llvm::Value* indexVal = index.v;
    if (indexVal->getType() != i64Ty) {
        indexVal = coerceValueToType(indexVal, i64Ty, b);
        if (!indexVal) return {};
    }

    // ─── Dispatch on array kind ───────────────────────────────────────────
    llvm::Value* elemPtr = nullptr;

    if (arrayTy->isFixed()) {
        // ─── Fixed array `[N]T` ───────────────────────────────────────────
        // The value is an `[N x T]` aggregate. Get a pointer to its first
        // element, then GEP by the index.
        llvm::ArrayType* arrTy = llvm::dyn_cast<llvm::ArrayType>(
            target.v->getType());
        if (!arrTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "fixed array value has an unexpected LLVM shape");
            return {};
        }

        llvm::Value* dataPtr = b.CreateConstGEP2_32(
            arrTy, target.v, 0, 0, "index.data");

        // ─── Compile-time bounds check ────────────────────────────────
        // If the index is a constant and `0 <= i < N`, the GEP is
        // provably in-bounds and no runtime check is needed.
        bool staticallyInBounds = false;
        if (auto* constIdx = llvm::dyn_cast<llvm::ConstantInt>(indexVal)) {
            uint64_t i = constIdx->getZExtValue();
            if (i < arrayTy->size) {
                staticallyInBounds = true;
            }
        }

        if (staticallyInBounds) {
            // Direct GEP. The optimiser will fold this into the constant
            // offset.
            elemPtr = b.CreateGEP(
                elemTy, dataPtr, indexVal, "index.ptr");
        } else {
            // Runtime check: `0 <= i && i < N`.
            llvm::Value* inBounds = emitFixedArrayBoundsCheck(
                indexVal, arrayTy->size, expr->loc);
            if (!inBounds) return {};
            // The bounds-check helper leaves the insertion point in the
            // success block. The GEP goes there.
            elemPtr = b.CreateGEP(
                elemTy, dataPtr, indexVal, "index.ptr");
        }
    } else if (arrayTy->isSlice()) {
        // ─── Slice `[_]T` ─────────────────────────────────────────────────
        // The value is `{ ptr, i64 len, i64 cap }`. Extract the data
        // pointer (field 0) and the length (field 1).
        llvm::StructType* sliceTy = llvm::dyn_cast<llvm::StructType>(
            target.v->getType());
        if (!sliceTy || sliceTy->getNumElements() != 3) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "slice value has an unexpected LLVM shape");
            return {};
        }

        llvm::Value* dataPtr = b.CreateExtractValue(
            target.v, 0, "index.slice.data");
        llvm::Value* len = b.CreateExtractValue(
            target.v, 1, "index.slice.len");

        // Runtime check: `0 <= i && i < len`.
        llvm::Value* inBounds = emitSliceBoundsCheck(
            indexVal, len, expr->loc);
        if (!inBounds) return {};

        elemPtr = b.CreateGEP(
            elemTy, dataPtr, indexVal, "index.ptr");
    } else if (arrayTy->isDynamic()) {
        // ─── Dynamic array `[*]T` ─────────────────────────────────────────
        // The current lowering is a bare `ptr` with no length. There's
        // nothing to bounds-check against, and no defined iteration
        // semantics yet.
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "indexing a dynamic array is not yet implemented");
        return {};
    } else {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "unsupported array kind in index expression");
        return {};
    }

    if (!elemPtr) return {};

    // ─── Load the element ─────────────────────────────────────────────────
    llvm::Value* loaded = b.CreateLoad(elemTy, elemPtr, "index.value");

    return Val{loaded, arrayTy->element, Own::Borrowed};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitSlice — slice construction
// ─────────────────────────────────────────────────────────────────────────────
//
// A slice expression `arr[lo..hi]` (or `arr[lo..<hi]`) produces a
// `{ ptr, i64 len, i64 cap }` value that views into `arr`'s storage. The
// slice doesn't own the buffer — dropping it does nothing (its
// `ResourceKind` is `None`). The backing array must outlive the slice.
//
// ─── Bounds ───────────────────────────────────────────────────────────────
// Sema doesn't fully check slice bounds (they depend on runtime lengths).
// The emitter emits a runtime check: `0 <= lo <= hi <= len`. On failure,
// panic unless the slice is inside a `??` context.
//
// ─── Construction Steps ───────────────────────────────────────────────────
//   1. Get the source array's data pointer.
//   2. Get the source array's length.
//   3. Evaluate `lo` and `hi` (defaulting to 0 and len).
//   4. Bounds-check `0 <= lo <= hi <= len`.
//   5. Compute `slicePtr = dataPtr + lo * sizeof(T)`.
//   6. Compute `sliceLen = hi - lo`.
//   7. Compute `sliceCap = len - lo`.
//   8. Build the `{ slicePtr, sliceLen, sliceCap }` value.

Val Emitter::emitSlice(SliceExprAST* expr) {
    assert(expr && "emitSlice() with null expression");

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    // ─── Target ───────────────────────────────────────────────────────────
    Val target = emit(expr->target);
    if (!target.isValid()) return {};

    ArrayTypeAST* arrayTy = expr->target->resolvedType
        ? (expr->target->resolvedType->isa<ArrayTypeAST>()
              ? expr->target->resolvedType->as<ArrayTypeAST>()
              : nullptr)
        : nullptr;
    if (!arrayTy) {
        program.diagnostics.errorAt(
            DiagCode::Sem_InvalidArrayElement, expr->target->loc,
            "slice target is not an array type");
        return {};
    }

    llvm::Type* elemTy = program.types().get(arrayTy->element);
    if (!elemTy) return {};

    // ─── Data pointer and length ──────────────────────────────────────────
    llvm::Value* dataPtr = nullptr;
    llvm::Value* len = nullptr;

    if (arrayTy->isFixed()) {
        llvm::ArrayType* arrTy = llvm::dyn_cast<llvm::ArrayType>(
            target.v->getType());
        if (!arrTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "fixed array value has an unexpected LLVM shape");
            return {};
        }
        dataPtr = b.CreateConstGEP2_32(
            arrTy, target.v, 0, 0, "slice.data");
        len = llvm::ConstantInt::get(
            i64Ty, static_cast<uint64_t>(arrayTy->size));
    } else if (arrayTy->isSlice()) {
        llvm::StructType* sliceTy = llvm::dyn_cast<llvm::StructType>(
            target.v->getType());
        if (!sliceTy || sliceTy->getNumElements() != 3) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "slice value has an unexpected LLVM shape");
            return {};
        }
        dataPtr = b.CreateExtractValue(target.v, 0, "slice.src.data");
        len = b.CreateExtractValue(target.v, 1, "slice.src.len");
        if (len->getType() != i64Ty) {
            len = coerceValueToType(len, i64Ty, b);
        }
    } else if (arrayTy->isDynamic()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "slicing a dynamic array is not yet implemented");
        return {};
    } else {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "unsupported array kind in slice expression");
        return {};
    }

    if (!dataPtr || !len) return {};

    // ─── Evaluate lo and hi ───────────────────────────────────────────────
    // Defaults: `lo = 0`, `hi = len`.
    llvm::Value* loVal = llvm::ConstantInt::get(i64Ty, 0);
    llvm::Value* hiVal = len;

    if (expr->start) {
        Val lo = emit(expr->start);
        if (!lo.isValid()) return {};
        loVal = lo.v;
        if (loVal->getType() != i64Ty) {
            loVal = coerceValueToType(loVal, i64Ty, b);
            if (!loVal) return {};
        }
    }

    if (expr->end) {
        Val hi = emit(expr->end);
        if (!hi.isValid()) return {};
        hiVal = hi.v;
        if (hiVal->getType() != i64Ty) {
            hiVal = coerceValueToType(hiVal, i64Ty, b);
            if (!hiVal) return {};
        }
    }

    // ─── Bounds check ─────────────────────────────────────────────────────
    // `0 <= lo && lo <= hi && hi <= len`.
    llvm::Value* loOk = b.CreateICmpSGE(
        loVal, llvm::ConstantInt::get(i64Ty, 0), "slice.lo.ok");
    llvm::Value* loHiOk = b.CreateICmpSLE(
        loVal, hiVal, "slice.lo_le_hi");
    llvm::Value* hiLenOk = b.CreateICmpSLE(
        hiVal, len, "slice.hi_le_len");
    llvm::Value* ok1 = b.CreateAnd(loOk, loHiOk, "slice.ok.1");
    llvm::Value* ok = b.CreateAnd(ok1, hiLenOk, "slice.ok");

    llvm::Function* fn = b.GetInsertBlock()->getParent();
    llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
        ctx, "slice.panic", fn);
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx, "slice.continue", fn);

    b.CreateCondBr(ok, continueBlock, panicBlock);

    b.SetInsertPoint(panicBlock);
    emitPanic(RuntimeErrorKind::SliceBoundsOutOfRange, expr->loc);

    b.SetInsertPoint(continueBlock);

    // ─── Compute slice fields ─────────────────────────────────────────────
    // slicePtr = dataPtr + lo
    // sliceLen = hi - lo
    // sliceCap = len - lo
    llvm::Value* slicePtr = b.CreateGEP(
        elemTy, dataPtr, loVal, "slice.ptr");
    llvm::Value* sliceLen = b.CreateSub(hiVal, loVal, "slice.len");
    llvm::Value* sliceCap = b.CreateSub(len, loVal, "slice.cap");

    // ─── Build the slice value ────────────────────────────────────────────
    llvm::StructType* resultTy = llvm::dyn_cast<llvm::StructType>(
        program.types().get(expr->resolvedType));
    if (!resultTy || resultTy->getNumElements() != 3) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "slice result type has an unexpected shape");
        return {};
    }

    llvm::Value* result = llvm::UndefValue::get(resultTy);
    result = b.CreateInsertValue(result, slicePtr, 0, "slice.set.ptr");
    result = b.CreateInsertValue(result, sliceLen, 1, "slice.set.len");
    result = b.CreateInsertValue(result, sliceCap, 2, "slice.set.cap");

    // A slice is a fresh value but owns nothing. The `Owned` tag here is
    // about the value being freshly constructed, not about a claim on the
    // backing buffer. `Ownership::drop` for a slice is a no-op because
    // the slice's `ResourceKind` is `None`.
    return Val{result, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFieldAccess — struct field read or enum variant access
// ─────────────────────────────────────────────────────────────────────────────
//
// Two modes, disambiguated by `expr->isEnumAccess`:
//
//   - Enum variant access (`Direction.North`): emit the variant's
//     `ConstantInt`. No storage; the value is a compile-time constant.
//
//   - Struct field access (`point.x`): GEP into the struct, load the
//     field. The result is `Borrowed`.
//
// ─── The Object's Place vs. Value ─────────────────────────────────────────
// A field access can be on an l-value or an r-value:
//
//   - `let p = Point { ... }; p.x` — `p` is a binding; `p.x` is an
//     l-value field access. The value form loads from `p`'s alloca, GEPs
//     to the field, and loads.
//
//   - `Point { ... }.x` — the object is an r-value. The value form is an
//     SSA struct; the field is extracted with `extractvalue`.
//
// The emitter dispatches on `expr->object->isLValue` (Sema set this) to
// choose between GEP+load and extractvalue. Both produce the same
// `Borrowed` `Val` from the caller's perspective.

Val Emitter::emitFieldAccess(FieldAccessExprAST* expr) {
    assert(expr && "emitFieldAccess() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Enum variant access ──────────────────────────────────────────────
    if (expr->isEnumAccess) {
        if (!expr->resolvedDecl
            || !expr->resolvedDecl->isa<EnumVariantAST>()) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "enum access resolved to a non-variant declaration");
            return {};
        }
        EnumVariantAST* variant = expr->resolvedDecl->as<EnumVariantAST>();
        llvm::Type* enumTy = program.types().get(expr->resolvedType);
        if (!enumTy || !enumTy->isIntegerTy()) {
            program.diagnostics.errorAt(
                DiagCode::Backend_InvalidIR, expr->loc,
                "enum variant has a non-integer type");
            return {};
        }
        llvm::Value* c = llvm::ConstantInt::get(
            enumTy,
            static_cast<uint64_t>(variant->value),
            /*isSigned=*/true);
        return Val{c, expr->resolvedType, Own::Owned};
    }

    // ─── Struct field access ──────────────────────────────────────────────
    if (!expr->resolvedDecl
        || !expr->resolvedDecl->isa<FieldDeclAST>()) {
        program.diagnostics.errorAt(
            DiagCode::Sem_FieldNotFound, expr->loc,
            "field '", program.pool.lookup(expr->fieldName),
            "' was not resolved");
        return {};
    }
    FieldDeclAST* field = expr->resolvedDecl->as<FieldDeclAST>();

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
    // Sema caches the field's index on the AST; the emitter reads it.
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

    // ─── Object value ─────────────────────────────────────────────────────
    Val object = emit(expr->object);
    if (!object.isValid()) return {};

    // ─── L-value object: GEP + load ───────────────────────────────────────
    // The object is storage (a binding, another field access, an index).
    // The value form of `p.x` is: load `p`'s storage to get the struct
    // value, then GEP into the alloca and load the field.
    //
    // Wait — that's wrong. If `p.x` is an l-value access, the emitter
    // should GEP directly into `p`'s storage and load from the field
    // pointer. Loading `p` first and then using `extractvalue` would be
    // the r-value form.
    //
    // The correct path: use `emitPlace(expr->object)` to get the object's
    // storage pointer, GEP to the field, load.
    if (expr->object->isLValue) {
        Place objPlace = emitPlace(expr->object);
        if (objPlace.isValid()) {
            llvm::Value* fieldPtr = b.CreateStructGEP(
                structTy,
                objPlace.ptr,
                static_cast<unsigned>(fieldIndex),
                "field." + program.pool.lookup(field->name));
            llvm::Type* fieldTy = structTy->getElementType(fieldIndex);
            llvm::Value* loaded = b.CreateLoad(
                fieldTy, fieldPtr,
                "field.value." + program.pool.lookup(field->name));
            return Val{loaded, field->type, Own::Borrowed};
        }
        // Fall through to the r-value path if `emitPlace` failed. This
        // shouldn't happen (Sema marks `isLValue` only for assignable
        // expressions), but falling back is safer than returning nothing.
    }

    // ─── R-value object: extractvalue ─────────────────────────────────────
    // The object is a value (a struct literal, a call returning a struct,
    // a field access chain). Extract the field directly.
    if (object.v->getType()->isStructTy()) {
        llvm::Value* extracted = b.CreateExtractValue(
            object.v,
            static_cast<unsigned>(fieldIndex),
            "field.value." + program.pool.lookup(field->name));
        return Val{extracted, field->type, Own::Borrowed};
    }

    // ─── Pointer object that's not an l-value ─────────────────────────────
    // A pointer from `#toRef` or a similar intrinsic. GEP + load.
    if (object.v->getType()->isPointerTy()) {
        llvm::Value* fieldPtr = b.CreateStructGEP(
            structTy,
            object.v,
            static_cast<unsigned>(fieldIndex),
            "field.ptr." + program.pool.lookup(field->name));
        llvm::Type* fieldTy = structTy->getElementType(fieldIndex);
        llvm::Value* loaded = b.CreateLoad(
            fieldTy, fieldPtr,
            "field.value." + program.pool.lookup(field->name));
        return Val{loaded, field->type, Own::Borrowed};
    }

    program.diagnostics.errorAt(
        DiagCode::Backend_CodegenError, expr->loc,
        "field access on a value of an unsupported shape");
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitModuleAccess — cross-module member access
// ─────────────────────────────────────────────────────────────────────────────
//
// Under the module-as-namespace model, a module's top-level state lives in
// a per-module global of the module's instance struct type. `mod:name`
// lowers to:
//
//     %inst   = load ptr, @__module_state_<mod>
//     %field  = gep %struct.module_<mod>, %inst, 0, <index>
//     %value  = load <fieldType>, %field
//
// ─── Function References Are Different ────────────────────────────────────
// A `mod:fnName` reference is resolved to the function's `llvm::Function*`
// via the module's symbol table. There's no instance field involved. The
// reference is `Borrowed` (a global symbol) and produces a bare function
// pointer for a `fn`-shaped function.
//
// ─── `cls`-Shaped Functions ───────────────────────────────────────────────
// A `cls`-shaped function's value is a runtime fat pointer constructed at
// the defining module's lowering time. There's no bare symbol for the
// importing module to look up. Sema rejects cross-module `cls` access;
// this emitter's job is just to fail loudly if it's reached.

Val Emitter::emitModuleAccess(ModuleAccessExprAST* expr) {
    assert(expr && "emitModuleAccess() with null expression");

    if (!expr->resolvedDecl) {
        program.diagnostics.errorAt(
            DiagCode::Sem_UndefinedMember, expr->loc,
            "module member '", program.pool.lookup(expr->moduleName),
            ":", program.pool.lookup(expr->memberName),
            "' was not resolved");
        return {};
    }

    llvm::IRBuilder<>& b = program.builder();

    // ─── Function reference ───────────────────────────────────────────────
    if (expr->resolvedDecl->isa<FuncDeclAST>()) {
        FuncDeclAST* fn = expr->resolvedDecl->as<FuncDeclAST>();

        // `cls`-shaped: rejected. Sema should have caught this; the
        // diagnostic is defensive.
        FuncShape shape = fn->funcType ? fn->funcType->shape : FuncShape::Fn;
        if (shape == FuncShape::Cls) {
            program.diagnostics.errorAt(
                DiagCode::Sem_GenericInstantiate, expr->loc,
                "cross-module access to 'cls' function '",
                program.pool.lookup(fn->name),
                "' is not supported — its value is a runtime-constructed "
                "fat pointer tied to the defining module's frame");
            return {};
        }

        // `fn`-shaped: resolve the `llvm::Function*` via the function
        // table. The declare pass created the prototype for every
        // function in every module, so a lookup here is a cache hit.
        llvm::Function* llvmFn = program.lookupFunction(fn);
        if (!llvmFn) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "function '", program.pool.lookup(fn->name),
                "' has no LLVM prototype in module '",
                program.pool.lookup(expr->moduleName), "'");
            return {};
        }
        return Val{llvmFn, expr->resolvedType, Own::Borrowed};
    }

    // ─── Variable reference ───────────────────────────────────────────────
    if (expr->resolvedDecl->isa<VarDeclAST>()) {
        VarDeclAST* var = expr->resolvedDecl->as<VarDeclAST>();

        // The variable must have a module-instance slot. A module-level
        // variable always does; a local reached via `mod:name` is a Sema
        // bug.
        if (var->moduleFieldIndex == SIZE_MAX) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "module member '", program.pool.lookup(expr->memberName),
                "' is not module-level");
            return {};
        }

        // The module instance layout. `DeclarePass` populated it via
        // `Types::moduleInstanceType`.
        ModuleAST* home = var->declaringModule;
        if (!home) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "module member '", program.pool.lookup(expr->memberName),
                "' has no home module");
            return {};
        }

        // ─── Module instance layout ───────────────────────────────────────
        // The layout gives the LLVM struct type and the field index. It
        // does NOT carry the state global's symbol name — that lives on
        // the manifest, for the host. Codegen derives the name the same
        // way `ModulePass::emitModuleStateGlobal` does:
        //
        //     "__module_state_" + sanitizeForLLVMSymbol(filePath)
        //
        // The derivation must match ModulePass's, or the global lookup
        // below will fail. Both use `sanitizeForLLVMSymbol` (or the
        // equivalent helper) on the file path.
        auto it = program.moduleLayouts().find(home);
        if (it == program.moduleLayouts().end() || !it->second.type) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "module instance for '", program.pool.lookup(expr->moduleName),
                "' has no LLVM type");
            return {};
        }
        ModuleInstanceLayout& layout = it->second;

        // ─── Locate the state global ──────────────────────────────────────
        // Use the same canonical derivation as ModulePass.
        std::string stateName = program.moduleStateSymbol(home);

        llvm::GlobalVariable* stateGlobal =
            program.module().getGlobalVariable(stateName, /*AllowInternal=*/true);
        if (!stateGlobal) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "module state global '", stateName, "' not found — "
                "the module pass did not emit it, or the naming "
                "derivation disagrees with DeclarePass/ModulePass");
            return {};
        }

        // ─── GEP into the instance struct ─────────────────────────────────
        llvm::Value* fieldPtr = b.CreateStructGEP(
            layout.type,
            stateGlobal,
            static_cast<unsigned>(var->moduleFieldIndex),
            "mod." + program.pool.lookup(var->name));

        llvm::Type* fieldTy = layout.type->getElementType(
            var->moduleFieldIndex);
        if (!fieldTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "module member '", program.pool.lookup(expr->memberName),
                "' has no LLVM field type");
            return {};
        }

        llvm::Value* loaded = b.CreateLoad(
            fieldTy, fieldPtr,
            "mod.value." + program.pool.lookup(var->name));

        return Val{loaded, var->type, Own::Borrowed};
    }

    program.diagnostics.errorAt(
        DiagCode::Sem_UndefinedMember, expr->loc,
        "module member '", program.pool.lookup(expr->memberName),
        "' has an unsupported declaration kind");
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitArenaAccess — Arena method calls
// ─────────────────────────────────────────────────────────────────────────────
//
// Arena access has two forms:
//
//   - Static: `Arena::create(size)`, `Arena::empty()`.
//   - Instance: `arena::alloc<Node>(count)`, `arena::reset()`,
//     `arena::descriptor()`, `arena::capacity()`, `arena::remaining()`,
//     `arena::isEmpty()`, `arena::space<T>()`, `arena::canFit<T>(count)`.
//
// Each form maps to one or more runtime calls. The emitter's job is to
// evaluate the arguments, call the appropriate runtime functions, and
// assemble the result (which is sometimes a struct — `Arena`,
// `ArenaDescriptor` — and sometimes a scalar).

Val Emitter::emitArenaAccess(ArenaAccessExprAST* expr) {
    assert(expr && "emitArenaAccess() with null expression");

    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);

    InternedString method = expr->methodName;

    // ─── Static forms ─────────────────────────────────────────────────────
    if (expr->isStatic) {
        // ─── Arena::create(size) -> Arena! ────────────────────────────────
        if (program.pool.lookupView(method) == "create") {
            if (expr->args.empty()) {
                program.diagnostics.errorAt(
                    DiagCode::Sem_ArgCountMismatch, expr->loc,
                    "Arena::create requires a size argument");
                return {};
            }

            Val size = emit(expr->args[0]);
            if (!size.isValid()) return {};
            llvm::Value* sizeVal = size.v;
            if (sizeVal->getType() != i64Ty) {
                sizeVal = coerceValueToType(sizeVal, i64Ty, b);
                if (!sizeVal) return {};
            }

            // Call `__lucid_arena_create(&desc, size)`.
            llvm::StructType* descTy = program.types().arenaDescriptorType();
            if (!descTy) return {};
            llvm::AllocaInst* descSlot = createEntryAlloca(
                descTy, "arena.desc");
            if (!descSlot) return {};

            program.abi().ArenaCreate(b, descSlot, sizeVal);

            // Read base and size from the descriptor.
            llvm::Value* base = b.CreateLoad(
                llvm::PointerType::get(ctx, 0),
                b.CreateStructGEP(descTy, descSlot, 0, "arena.desc.base.gep"),
                "arena.base");
            llvm::Value* actualSize = b.CreateLoad(
                i64Ty,
                b.CreateStructGEP(descTy, descSlot, 1, "arena.desc.size.gep"),
                "arena.size");

            // Build the Arena value: { ptr base, i64 size, i64 cursor }.
            llvm::StructType* arenaTy = program.types().arenaType();
            if (!arenaTy) return {};
            llvm::Value* arena = llvm::UndefValue::get(arenaTy);
            arena = b.CreateInsertValue(arena, base, 0, "arena.set.base");
            arena = b.CreateInsertValue(arena, actualSize, 1, "arena.set.size");
            arena = b.CreateInsertValue(
                arena, llvm::ConstantInt::get(i64Ty, 0), 2, "arena.set.cursor");

            // Wrap in the fallible result: `{ i8 tag, Arena }`.
            //
            // Tag convention: 0 = err, 1 = value. The `create` runtime
            // leaves the descriptor empty on failure (base == null), so
            // the tag is 1 iff base != null.
            llvm::StructType* resultTy = llvm::dyn_cast<llvm::StructType>(
                program.types().get(expr->resolvedType));
            if (!resultTy || resultTy->getNumElements() != 2) {
                program.diagnostics.errorAt(
                    DiagCode::Backend_CodegenError, expr->loc,
                    "Arena::create result has an unexpected shape");
                return {};
            }

            llvm::Value* isSuccess = b.CreateICmpNE(
                base,
                llvm::ConstantPointerNull::get(
                    llvm::PointerType::get(ctx, 0)),
                "arena.create.ok");
            llvm::Value* tag = b.CreateSelect(
                isSuccess,
                llvm::ConstantInt::get(i8Ty, 1),
                llvm::ConstantInt::get(i8Ty, 0),
                "arena.create.tag");

            llvm::Value* result = llvm::UndefValue::get(resultTy);
            result = b.CreateInsertValue(result, tag, 0, "result.tag");
            result = b.CreateInsertValue(result, arena, 1, "result.arena");
            return Val{result, expr->resolvedType, Own::Owned};
        }

        // ─── Arena::empty() -> Arena ──────────────────────────────────────
        if (program.pool.lookupView(method) == "empty") {
            llvm::StructType* arenaTy = program.types().arenaType();
            if (!arenaTy) return {};
            llvm::Value* empty = llvm::ConstantAggregateZero::get(arenaTy);
            return Val{empty, expr->resolvedType, Own::Owned};
        }

        program.diagnostics.errorAt(
            DiagCode::Sem_UnknownMethod, expr->loc,
            "unknown Arena static method '",
            program.pool.lookup(method), "'");
        return {};
    }

    // ─── Instance forms: arena::method(...) ───────────────────────────────
    // The receiver is `expr->arenaExpr`. It must be an `&Arena` — an
    // l-value place holding the arena value, whose address the runtime
    // takes. Sema guarantees the shape.

    // Get the arena's storage pointer. The runtime functions take
    // `ArenaPtr` (a `LucidArena*`), so the emitter needs the address, not
    // the value.
    Place arenaPlace = emitPlace(expr->arenaExpr);
    if (!arenaPlace.isValid()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "Arena method receiver is not an l-value");
        return {};
    }
    llvm::Value* arenaPtr = arenaPlace.ptr;

    // ─── arena::alloc<T>(count) -> [_]T ───────────────────────────────────
    if (program.pool.lookupView(method) == "alloc") {
        if (expr->genericArgs.empty()) {
            program.diagnostics.errorAt(
                DiagCode::Sem_GenericInstantiate, expr->loc,
                "arena::alloc requires a type argument");
            return {};
        }

        TypeAST* elemTy = expr->genericArgs[0];
        uint64_t elemSize = program.types().sizeOf(elemTy);
        uint64_t elemAlign = program.types().alignOf(elemTy);
        if (elemSize == 0) elemSize = 1;

        llvm::Value* count = llvm::ConstantInt::get(i64Ty, 1);
        if (!expr->args.empty()) {
            Val c = emit(expr->args[0]);
            if (!c.isValid()) return {};
            count = c.v;
            if (count->getType() != i64Ty) {
                count = coerceValueToType(count, i64Ty, b);
                if (!count) return {};
            }
        }

        llvm::Value* totalSize = b.CreateMul(
            count,
            llvm::ConstantInt::get(i64Ty, elemSize),
            "arena.alloc.total_size");

        llvm::Value* data = program.abi().ArenaAlloc(
            b, arenaPtr, totalSize,
            llvm::ConstantInt::get(i64Ty, elemAlign));

        // Bounds-check: null on failure.
        llvm::Function* fn = b.GetInsertBlock()->getParent();
        llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
            ctx, "arena.alloc.panic", fn);
        llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
            ctx, "arena.alloc.continue", fn);

        llvm::Value* isNull = b.CreateIsNull(data, "arena.alloc.failed");
        b.CreateCondBr(isNull, panicBlock, continueBlock);

        b.SetInsertPoint(panicBlock);
        emitPanic(RuntimeErrorKind::ArenaOutOfCapacity, expr->loc);

        b.SetInsertPoint(continueBlock);

        // Build the slice result: { data, count, count }.
        llvm::StructType* resultTy = llvm::dyn_cast<llvm::StructType>(
            program.types().get(expr->resolvedType));
        if (!resultTy || resultTy->getNumElements() != 3) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "arena::alloc result has an unexpected shape");
            return {};
        }
        llvm::Value* slice = llvm::UndefValue::get(resultTy);
        slice = b.CreateInsertValue(slice, data, 0, "slice.data");
        slice = b.CreateInsertValue(slice, count, 1, "slice.len");
        slice = b.CreateInsertValue(slice, count, 2, "slice.cap");
        return Val{slice, expr->resolvedType, Own::Owned};
    }

    // ─── arena::reset() ───────────────────────────────────────────────────
    if (program.pool.lookupView(method) == "reset") {
        program.abi().ArenaReset(b, arenaPtr);
        return {};
    }

    // ─── arena::descriptor() -> ArenaDescriptor ───────────────────────────
    if (program.pool.lookupView(method) == "descriptor") {
        llvm::StructType* arenaTy = program.types().arenaType();
        llvm::StructType* descTy = program.types().arenaDescriptorType();
        if (!arenaTy || !descTy) return {};

        // Load base and size from the arena.
        llvm::Value* base = b.CreateLoad(
            llvm::PointerType::get(ctx, 0),
            b.CreateStructGEP(arenaTy, arenaPtr, 0, "arena.base.gep"),
            "arena.base");
        llvm::Value* size = b.CreateLoad(
            i64Ty,
            b.CreateStructGEP(arenaTy, arenaPtr, 1, "arena.size.gep"),
            "arena.size");

        llvm::Value* desc = llvm::UndefValue::get(descTy);
        desc = b.CreateInsertValue(desc, base, 0, "desc.base");
        desc = b.CreateInsertValue(desc, size, 1, "desc.size");
        return Val{desc, expr->resolvedType, Own::Owned};
    }

    // ─── arena::capacity() / remaining() / space() ────────────────────────
    if (program.pool.lookupView(method) == "capacity") {
        llvm::Value* v = program.abi().ArenaCapacity(b, arenaPtr);
        return Val{v, expr->resolvedType, Own::Owned};
    }
    if (program.pool.lookupView(method) == "remaining") {
        llvm::Value* v = program.abi().ArenaRemaining(b, arenaPtr);
        return Val{v, expr->resolvedType, Own::Owned};
    }
    if (program.pool.lookupView(method) == "space") {
        if (expr->genericArgs.empty()) {
            program.diagnostics.errorAt(
                DiagCode::Sem_GenericInstantiate, expr->loc,
                "arena::space requires a type argument");
            return {};
        }
        uint64_t elemSize = program.types().sizeOf(expr->genericArgs[0]);
        if (elemSize == 0) elemSize = 1;
        llvm::Value* v = program.abi().ArenaSpace(
            b, arenaPtr, llvm::ConstantInt::get(i64Ty, elemSize));
        return Val{v, expr->resolvedType, Own::Owned};
    }

    // ─── arena::isEmpty() -> bool ─────────────────────────────────────────
    if (program.pool.lookupView(method) == "isEmpty") {
        llvm::Value* v = program.abi().ArenaIsEmpty(b, arenaPtr);
        return Val{v, expr->resolvedType, Own::Owned};
    }

    // ─── arena::canFit<T>(count) -> bool ──────────────────────────────────
    if (program.pool.lookupView(method) == "canFit") {
        if (expr->genericArgs.empty() || expr->args.empty()) {
            program.diagnostics.errorAt(
                DiagCode::Sem_ArgCountMismatch, expr->loc,
                "arena::canFit requires a type argument and a count");
            return {};
        }
        uint64_t elemSize = program.types().sizeOf(expr->genericArgs[0]);
        if (elemSize == 0) elemSize = 1;

        Val c = emit(expr->args[0]);
        if (!c.isValid()) return {};
        llvm::Value* count = c.v;
        if (count->getType() != i64Ty) {
            count = coerceValueToType(count, i64Ty, b);
            if (!count) return {};
        }
        llvm::Value* v = program.abi().ArenaCanFit(
            b, arenaPtr,
            llvm::ConstantInt::get(i64Ty, elemSize),
            count);
        return Val{v, expr->resolvedType, Own::Owned};
    }

    program.diagnostics.errorAt(
        DiagCode::Sem_UnknownMethod, expr->loc,
        "unknown Arena method '", program.pool.lookup(method), "'");
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Bounds-check helpers
// ─────────────────────────────────────────────────────────────────────────────
//
// Two small helpers shared by `emitIndex`. Both:
//
//   1. Compute an `i1` "in bounds" predicate.
//   2. Branch to a panic block on failure.
//   3. Emit the panic and `unreachable` in the panic block.
//   4. Leave the builder in the success block, so the caller appends the
//      GEP and load there.
//
// Both return the `i1` predicate (mainly so the caller can reuse it if
// needed, e.g. to fold into a select). Neither returns a null on success —
// they return the predicate value.

llvm::Value* Emitter::emitFixedArrayBoundsCheck(llvm::Value* index,
                                                 uint64_t size,
                                                 SourceLocation loc) {
    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* idxTy = index->getType();

    llvm::Value* loOk = b.CreateICmpSGE(
        index, llvm::ConstantInt::get(idxTy, 0), "idx.lo_ok");
    llvm::Value* hiOk = b.CreateICmpSLT(
        index, llvm::ConstantInt::get(idxTy, size), "idx.hi_ok");
    llvm::Value* inBounds = b.CreateAnd(loOk, hiOk, "idx.in_bounds");

    llvm::Function* fn = b.GetInsertBlock()->getParent();
    llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
        ctx, "idx.panic", fn);
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx, "idx.continue", fn);

    b.CreateCondBr(inBounds, continueBlock, panicBlock);

    b.SetInsertPoint(panicBlock);
    emitPanic(RuntimeErrorKind::ArrayIndexOutOfBounds, loc);

    b.SetInsertPoint(continueBlock);
    return inBounds;
}

llvm::Value* Emitter::emitSliceBoundsCheck(llvm::Value* index,
                                            llvm::Value* len,
                                            SourceLocation loc) {
    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* idxTy = index->getType();

    llvm::Value* loOk = b.CreateICmpSGE(
        index, llvm::ConstantInt::get(idxTy, 0), "idx.lo_ok");
    llvm::Value* hiOk = b.CreateICmpSLT(index, len, "idx.hi_ok");
    llvm::Value* inBounds = b.CreateAnd(loOk, hiOk, "idx.in_bounds");

    llvm::Function* fn = b.GetInsertBlock()->getParent();
    llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
        ctx, "idx.panic", fn);
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx, "idx.continue", fn);

    b.CreateCondBr(inBounds, continueBlock, panicBlock);

    b.SetInsertPoint(panicBlock);
    emitPanic(RuntimeErrorKind::ArrayIndexOutOfBounds, loc);

    b.SetInsertPoint(continueBlock);
    return inBounds;
}

} // namespace codegen