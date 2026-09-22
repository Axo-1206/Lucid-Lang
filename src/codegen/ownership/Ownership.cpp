/// @file codegen/ownership/Ownership.cpp
/// @brief Implementation of the ownership engine.

#include "Ownership.hpp"
#include "DropGlue.hpp"

#include "codegen/Program.hpp"
#include "codegen/Abi.hpp"
#include "codegen/Types.hpp"
#include "codegen/LLVMTypeHelpers.hpp"

#include "core/ast/ResourceKind.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Ownership::Ownership(ProgramState& program_)
    : program(program_)
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Null-Checked Call Helper
// ─────────────────────────────────────────────────────────────────────────────

void Ownership::emitNullCheckedCall(llvm::Value* arg,
                                     llvm::Function* fn,
                                     llvm::IRBuilder<>& builder,
                                     const llvm::Twine& prefix) {
    // Precondition: the builder has an active insertion point. This is
    // guaranteed by the call sites: `emitNullCheckedCall` is only called
    // from `dropRefcounted`, `dropOwnedBuffer`, and `dropArena`, all of
    // which run while emitting an aggregate's drop glue inside a live
    // function body. Reaching it without an insertion point would be a
    // caller bug, not a condition to handle here.
    llvm::BasicBlock* cur = builder.GetInsertBlock();
    assert(cur && "emitNullCheckedCall() called with no insertion point");
    llvm::Function* func = builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* callBlock = llvm::BasicBlock::Create(builder.getContext(), prefix + ".call", func);
    llvm::BasicBlock* skipBlock = llvm::BasicBlock::Create(builder.getContext(), prefix + ".skip", func);

    llvm::Value* isNull = builder.CreateIsNull(arg, prefix + ".is_null");
    builder.CreateCondBr(isNull, skipBlock, callBlock);

    builder.SetInsertPoint(callBlock);
    builder.CreateCall(fn, {arg});
    builder.CreateBr(skipBlock);

    builder.SetInsertPoint(skipBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// Refcounted Drop
// ─────────────────────────────────────────────────────────────────────────────

void Ownership::dropRefcounted(llvm::Value* fatPtr,
                               llvm::IRBuilder<>& builder) {
    llvm::Value* envPtr =
        builder.CreateExtractValue(fatPtr, 1, "closure_env_to_release");
    llvm::Function* releaseFn =
        program.abi().declareOrGet(RuntimeFn::ReleaseEnv);
    emitNullCheckedCall(envPtr, releaseFn, builder, "release_env");
}

// ─────────────────────────────────────────────────────────────────────────────
// OwnedBuffer Drop
// ─────────────────────────────────────────────────────────────────────────────
//
// A string is `{ ptr data, i64 len, i64 cap }`. The data pointer is field
// 0; the cap is field 2. A `cap == 0` value is a static string literal —
// its data pointer points into a private global, and freeing it would
// corrupt the allocator. The drop path checks the cap before freeing.
//
// ─── Dynamic Arrays and the Struct-Shape Assumption ──────────────────────
// A dynamic array `[*]T` is also an `OwnedBuffer` by classification, but
// today `Types::arrayType` lowers it to a bare `ptr`, not to the
// `{ ptr, i64, i64 }` shape. A bare `ptr` has no `cap` field, so the
// static-vs-heap check can't run.
//
// The assertion at the top of this function catches that case. If it
// fires, a dynamic array reached the drop path before the array-lowering
// fix landed. The fix is to lower `[*]T` to the same three-field shape as
// `string`; that change ripples through the array emitters and `Types`.
// Until it lands, no compilable program constructs a dynamic array, so
// the assertion is a tripwire, not a live failure.

void Ownership::dropOwnedBuffer(llvm::Value* buffer,
                                 llvm::IRBuilder<>& builder) {
    llvm::Type* bufferTy = buffer->getType();
    assert(bufferTy->isStructTy() && bufferTy->getStructNumElements() == 3
           && "dropOwnedBuffer received a non-struct value — a dynamic "
              "array reached the drop path before the array-lowering fix "
              "landed; see the comment in Types::arrayType");

    llvm::Value* dataPtr =
        builder.CreateExtractValue(buffer, 0, "buffer_data_to_release");
    llvm::Value* cap =
        builder.CreateExtractValue(buffer, 2, "buffer_cap_to_release");

    llvm::Value* isStatic =
        builder.CreateICmpEQ(cap,
                             llvm::ConstantInt::get(cap->getType(), 0),
                             "buffer_is_static");

    llvm::Function* func = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* freeBlock =
        llvm::BasicBlock::Create(builder.getContext(), "free_buffer", func);
    llvm::BasicBlock* skipBlock =
        llvm::BasicBlock::Create(builder.getContext(), "skip_free", func);

    builder.CreateCondBr(isStatic, skipBlock, freeBlock);

    builder.SetInsertPoint(freeBlock);
    llvm::Function* freeFn = program.abi().declareOrGet(RuntimeFn::Free);
    emitNullCheckedCall(dataPtr, freeFn, builder, "release_buffer");
    builder.CreateBr(skipBlock);

    builder.SetInsertPoint(skipBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// Arena Drop
// ─────────────────────────────────────────────────────────────────────────────

void Ownership::dropArena(llvm::Value* arena, llvm::IRBuilder<>& builder) {
    llvm::Value* basePtr =
        builder.CreateExtractValue(arena, 0, "arena_base_to_release");
    llvm::Function* freeFn = program.abi().declareOrGet(RuntimeFn::Free);
    emitNullCheckedCall(basePtr, freeFn, builder, "release_arena");
}

// ─────────────────────────────────────────────────────────────────────────────
// drop — the single free decision point
// ─────────────────────────────────────────────────────────────────────────────

void Ownership::drop(TypeAST* type,
                     llvm::Value* value,
                     llvm::IRBuilder<>& builder) {
    if (!type || !value) return;

    ResourceKind kind = classifyResourceKind(type);

    switch (kind) {
        case ResourceKind::None:
            return;

        case ResourceKind::Refcounted:
            dropRefcounted(value, builder);
            return;

        case ResourceKind::OwnedBuffer:
            dropOwnedBuffer(value, builder);
            return;

        case ResourceKind::Arena:
            dropArena(value, builder);
            return;

        case ResourceKind::Handle:
            // Future<T>, Thread<T>.
            //
            // The no-op is correct ONLY because Sema enforces a compile-time
            // linearity invariant: a live Thread<T>/Future<T> reaching scope
            // exit without being consumed by await/join is a compile error, so
            // by the time this drop runs, the handle was already consumed and
            // its box freed by the await/join site.
            //
            // If Sema's check is ever relaxed — a new language feature that
            // permits a handle to escape, a bug in the linearity pass — this
            // no-op silently leaks the handle's box. There is no runtime
            // signal: the value is a raw pointer and the drop has no way to
            // know whether it was consumed.
            //
            // The corresponding invariant on the Sema side is documented in
            // the grammar under "Future<T> — Linear Value Rules" and
            // "Thread<T>". Any change to that rule must revisit this case.
            return;

        case ResourceKind::Aggregate: {
            llvm::Function* glue = dropGlueFor(type);
            if (!glue) return;

            llvm::AllocaInst* slot = createEntryBlockAlloca(
                builder, value->getType(), "aggregate_slot");
            if (!slot) return;
            builder.CreateStore(value, slot);
            builder.CreateCall(glue, {slot});
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// retain — the retain half of Pattern A
// ─────────────────────────────────────────────────────────────────────────────

void Ownership::retain(TypeAST* type,
                       llvm::Value* value,
                       llvm::IRBuilder<>& builder) {
    if (!type || !value) return;

    ResourceKind kind = classifyResourceKind(type);

    switch (kind) {
        case ResourceKind::Refcounted: {
            llvm::Value* envPtr =
                builder.CreateExtractValue(value, 1, "closure_env_to_retain");
            llvm::Function* retainFn =
                program.abi().declareOrGet(RuntimeFn::RetainEnv);
            emitNullCheckedCall(envPtr, retainFn, builder, "retain_env");
            return;
        }

        case ResourceKind::OwnedBuffer:
        case ResourceKind::Arena:
        case ResourceKind::Handle:
        case ResourceKind::None:
        case ResourceKind::Aggregate:
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// intoOwned — the single copy decision point
// ─────────────────────────────────────────────────────────────────────────────

Val Ownership::intoOwned(Val val, llvm::IRBuilder<>& builder) {
    if (!val.isValid()) return val;

    if (val.own == Own::Owned) {
        return val;
    }

    ResourceKind kind = classifyResourceKind(val.ty);

    switch (kind) {
        case ResourceKind::None: {
            Val result = val;
            result.own = Own::Owned;
            return result;
        }

        case ResourceKind::Refcounted: {
            retain(val.ty, val.v, builder);
            Val result = val;
            result.own = Own::Owned;
            return result;
        }

        case ResourceKind::OwnedBuffer: {
            // Static string: bit copy. Non-static: deep copy via
            // str_concat(empty, src).
            llvm::Value* cap =
                builder.CreateExtractValue(val.v, 2, "buffer_cap");
            llvm::Value* isStatic =
                builder.CreateICmpEQ(cap,
                                     llvm::ConstantInt::get(cap->getType(), 0),
                                     "buffer_is_static");

            llvm::Function* func = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* staticBlock =
                llvm::BasicBlock::Create(builder.getContext(), "static_copy", func);
            llvm::BasicBlock* copyBlock =
                llvm::BasicBlock::Create(builder.getContext(), "deep_copy", func);
            llvm::BasicBlock* mergeBlock =
                llvm::BasicBlock::Create(builder.getContext(), "copy_merge", func);

            builder.CreateCondBr(isStatic, staticBlock, copyBlock);

            builder.SetInsertPoint(staticBlock);
            builder.CreateBr(mergeBlock);

            builder.SetInsertPoint(copyBlock);

            llvm::StructType* strTy = program.types().stringType();

            llvm::AllocaInst* outSlot = createEntryBlockAlloca(
                builder, strTy, "str_dup_out");
            llvm::AllocaInst* emptySlot = createEntryBlockAlloca(
                builder, strTy, "str_dup_empty");
            builder.CreateStore(
                llvm::ConstantAggregateZero::get(strTy), emptySlot);
            llvm::AllocaInst* srcSlot = createEntryBlockAlloca(
                builder, strTy, "str_dup_src");
            builder.CreateStore(val.v, srcSlot);

            llvm::Function* concatFn =
                program.abi().declareOrGet(RuntimeFn::StrConcat);
            builder.CreateCall(concatFn, {outSlot, emptySlot, srcSlot});

            llvm::Value* copied =
                builder.CreateLoad(strTy, outSlot, "str_dup_result");
            builder.CreateBr(mergeBlock);

            builder.SetInsertPoint(mergeBlock);
            llvm::PHINode* result =
                builder.CreatePHI(strTy, 2, "str_owned_value");
            result->addIncoming(val.v, staticBlock);
            result->addIncoming(copied, copyBlock);

            Val out;
            out.v = result;
            out.ty = val.ty;
            out.own = Own::Owned;
            return out;
        }

        case ResourceKind::Aggregate: {
            llvm::Function* glue = copyGlueFor(val.ty);
            if (!glue) {
                // Copy glue could not be generated for this aggregate
                // type. This is either a tagged slot whose inner type
                // owns a resource (not yet supported by the glue
                // generator), a fixed array of resource-bearing
                // elements (also unsupported), or a genuine generation
                // failure.
                //
                // Returning a Val with a flipped ownership tag would
                // produce two "Owned" bindings to the same storage.
                // Return an invalid Val instead; the caller is
                // expected to check and bail.
                //
                // TODO: implement glue for tagged slots and fixed
                // arrays of resources. Until then, `??` on a Borrowed
                // value of one of those types will fail rather than
                // silently corrupt.
                assert(false && "copy glue not available for an "
                                 "aggregate type that requires one — "
                                 "see DropGlue.cpp's getAggregateInfo");
                return Val{};
            }

            llvm::AllocaInst* srcSlot = createEntryBlockAlloca(
                builder, val.v->getType(), "copy_src");
            if (!srcSlot) {
                // Stack allocation failed. This is an internal
                // compiler error (no insertion point); returning an
                // invalid Val is the correct signal.
                assert(false && "createEntryBlockAlloca failed in "
                                 "intoOwned's Aggregate branch");
                return Val{};
            }
            builder.CreateStore(val.v, srcSlot);

            llvm::Value* dstSlot =
                builder.CreateCall(glue, {srcSlot}, "copy_dst");
            llvm::Value* copied = builder.CreateLoad(
                val.v->getType(), dstSlot, "copy_result");

            Val result;
            result.v = copied;
            result.ty = val.ty;
            result.own = Own::Owned;
            return result;
        }

        case ResourceKind::Arena:
        case ResourceKind::Handle: {
            // Sema guarantees this branch is unreachable: an Arena or a
            // Thread<T>/Future<T> handle is a linear value and cannot be
            // copied, so `intoOwned` should never see a Borrowed one. If
            // it does, Sema's linearity check let a copy through.
            //
            // Returning a Val with a flipped ownership tag would produce
            // two "Owned" bindings to the same storage — a double-free
            // for Arena (both `dropArena` calls free `base`), a
            // double-consume race for Handle (two `await`/`join` sites
            // race for the same box). Return an invalid Val instead;
            // every caller is expected to check `isValid()` and bail
            // before doing anything with the result.
            //
            // Returning the input unchanged (still `Borrowed`) would be
            // marginally better than flipping the tag — the caller sees
            // "I couldn't acquire a claim" — but the caller would still
            // store a value it doesn't own. An invalid Val forces the
            // caller's check to fire, which is the correct contract.
            assert(false && "intoOwned() called on a Borrowed Arena/Handle — "
                             "Sema should have rejected this copy");
            return Val{};
        }
    }

    return val;
}

// ─────────────────────────────────────────────────────────────────────────────
// Aggregate Glue Accessors
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* Ownership::dropGlueFor(TypeAST* type) {
    return generateDropGlue(*this, type, program);
}

llvm::Function* Ownership::copyGlueFor(TypeAST* type) {
    return generateCopyGlue(*this, type, program);
}

} // namespace codegen