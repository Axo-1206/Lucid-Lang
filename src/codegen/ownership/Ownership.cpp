/// @file codegen/ownership/Ownership.cpp
/// @brief Implementation of the ownership engine.

#include "Ownership.hpp"
#include "DropGlue.hpp"

#include "codegen/Program.hpp"
#include "codegen/Abi.hpp"
#include "codegen/Types.hpp"

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
// Entry-Block Alloca Helper
// ─────────────────────────────────────────────────────────────────────────────
//
// Every alloca the ownership engine creates is placed in the current
// function's entry block. Creating allocas in the current block would make
// each drop inside a loop allocate a new stack slot per iteration, and the
// slots would accumulate until the function returns.

namespace {

llvm::AllocaInst* createEntryBlockAlloca(llvm::IRBuilder<>& builder,
                                          llvm::Type* ty,
                                          const llvm::Twine& name) {
    llvm::Function* func = builder.GetInsertBlock()
        ? builder.GetInsertBlock()->getParent()
        : nullptr;
    if (!func) return nullptr;

    llvm::BasicBlock& entry = func->getEntryBlock();
    llvm::IRBuilderBase::InsertPoint saved = builder.saveIP();
    builder.SetInsertPoint(&entry, entry.getFirstInsertionPt());

    llvm::AllocaInst* alloca = builder.CreateAlloca(ty, nullptr, name);

    builder.restoreIP(saved);
    return alloca;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Null-Checked Call Helper
// ─────────────────────────────────────────────────────────────────────────────

void Ownership::emitNullCheckedCall(llvm::Value* arg,
                                     llvm::Function* fn,
                                     llvm::IRBuilder<>& builder,
                                     const llvm::Twine& prefix) {
    llvm::Function* func = builder.GetInsertBlock()->getParent();
    if (!func) return;

    llvm::BasicBlock* callBlock =
        llvm::BasicBlock::Create(builder.getContext(), prefix + ".call", func);
    llvm::BasicBlock* skipBlock =
        llvm::BasicBlock::Create(builder.getContext(), prefix + ".skip", func);

    llvm::Value* isNull =
        builder.CreateIsNull(arg, prefix + ".is_null");
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
            // Future<T>, Thread<T>. Sema guarantees the handle was
            // consumed by await/join before scope exit.
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
                Val result = val;
                result.own = Own::Owned;
                return result;
            }

            llvm::AllocaInst* srcSlot = createEntryBlockAlloca(
                builder, val.v->getType(), "copy_src");
            if (!srcSlot) return val;
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
            Val result = val;
            result.own = Own::Owned;
            return result;
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