/// @file codegen/ownership/Ownership.cpp
/// @brief Implementation of the ownership engine.

#include "Ownership.hpp"
#include "DropGlue.hpp"

#include "codegen/Program.hpp"
#include "codegen/Abi.hpp"
#include "codegen/Types.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/context/CodeGenContext.hpp"

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
//
// Emits:
//     %is_null = icmp eq ptr %arg, null
//     br i1 %is_null, label %skip, label %call
//   call:
//     call void @fn(ptr %arg)
//     br label %skip
//   skip:
//
// The runtime functions this wraps — `__lucid_release_env`, `__lucid_free`,
// `__lucid_arena_free` — are all null-safe. The null check is redundant at
// the runtime level. It's kept because:
//
//   1. It makes the generated IR self-evidently correct on inspection:
//      a reader sees "if null, skip" and knows no call happens on null.
//   2. It avoids a call for the common case of an unset or empty binding.
//
// A future pass could remove it if IR size becomes a concern. Today it's
// cheap and readable.

void Ownership::emitNullCheckedCall(llvm::Value* arg,
                                     llvm::Function* fn,
                                     llvm::IRBuilder<>& builder,
                                     const llvm::Twine& prefix) {
    llvm::Function* func = builder.GetInsertBlock()->getParent();
    if (!func) return;  // caller bug; nothing to attach blocks to

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
//
// A `cls` value is a `lucid.Closure` fat pointer: `{ ptr fn, ptr env }`.
// The env pointer is field 1. Releasing the env decrements its refcount;
// if it hits zero, the env's drop function runs and the env is freed.
//
// The env pointer may be null (a non-capturing closure coerced to `cls`
// has a null env). `__lucid_release_env` is null-safe, but the
// `emitNullCheckedCall` wrapper skips the call entirely when it's null.

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
// A string or dynamic array is `{ ptr data, i64 len, i64 cap }`. The data
// pointer is field 0; the cap is field 2. A `cap == 0` value is a static
// string literal — its data pointer points into a private global, and
// freeing it would corrupt the allocator. The drop path checks the cap
// before freeing.
//
// The current dynamic-array lowering is a bare `ptr` (see Types.cpp), not
// a three-field struct. When the dynamic-array lowering is fixed to match
// the string's shape, this function will handle it correctly without
// change. Until then, dynamic arrays that reach this function will be a
// bare pointer, and `CreateExtractValue` will fail. This is a known
// inconsistency that Task 5's emitter rewrite will resolve by lowering
// dynamic arrays to the same three-field shape.

void Ownership::dropOwnedBuffer(llvm::Value* buffer,
                                 llvm::IRBuilder<>& builder) {
    llvm::Value* dataPtr =
        builder.CreateExtractValue(buffer, 0, "buffer_data_to_release");
    llvm::Value* cap =
        builder.CreateExtractValue(buffer, 2, "buffer_cap_to_release");

    // If cap == 0, this is a static literal. Skip the free.
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
    // Now null-check the data pointer inside the free block.
    emitNullCheckedCall(dataPtr, freeFn, builder, "release_buffer");
    builder.CreateBr(skipBlock);

    builder.SetInsertPoint(skipBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// Arena Drop
// ─────────────────────────────────────────────────────────────────────────────
//
// An Arena is `{ ptr base, i64 size, i64 cursor }`. The base pointer is
// field 0. Freeing the base releases the arena's backing memory.
//
// `Ownership::drop` frees the base via `__lucid_free`, not
// `__lucid_arena_free`. The reason is the single-allocator invariant: every
// heap allocation goes through `lucid_alloc`, and every free goes through
// `lucid_free`. The `Arena` runtime entry points that allocate and free are
// thin wrappers that call the same allocator; a direct `free(base)` is
// equivalent and avoids a runtime call.
//
// `__lucid_arena_free` also resets the arena's `size` and `cursor` fields,
// which is important for the arena struct's internal state. But the arena
// value being dropped here is about to go out of scope; its fields won't
// be read again. So resetting them is unnecessary.

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
            // Primitives, `fn` pointers, references, enum variants. Nothing
            // to release.
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
            // Future<T>, Thread<T>. Sema guarantees the handle was consumed
            // by await/join before the binding's scope exits. If codegen
            // reaches here with a live handle, it's a Sema bug — but
            // dropping a handle is a no-op at runtime, so the consequence
            // is a leaked handle, not corruption.
            return;

        case ResourceKind::Aggregate: {
            llvm::Function* glue = dropGlueFor(type);
            if (!glue) {
                // Glue generation failed. Report via a comment in the IR
                // and skip the drop — losing a drop is a leak, not a crash.
                // The emitter's caller will surface the diagnostic; here
                // we just don't emit the call.
                return;
            }
            // The glue takes a pointer to the aggregate. The value we have
            // is the aggregate by value, so we need to spill it to an
            // alloca and pass the pointer.
            llvm::AllocaInst* slot = builder.CreateAlloca(
                value->getType(), nullptr, "aggregate_slot");
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
            // Deep-copy semantics: the receiver acquires its own buffer via
            // a copy, not via a retain. `intoOwned` handles the copy; this
            // method is only for the refcounted case, so nothing to do.
            return;

        case ResourceKind::Arena:
        case ResourceKind::Handle:
            // Linear types; Sema rejects copies. If codegen reaches here,
            // it's a Sema bug. No-op rather than crash.
            return;

        case ResourceKind::None:
        case ResourceKind::Aggregate:
            // `None` has nothing to retain.
            // `Aggregate` retain is per-field, done by the generated
            // `__copy_<type>`, not by this method. `intoOwned` for an
            // aggregate calls the copy glue; this method is not the right
            // entry point for aggregates.
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// intoOwned — the single copy decision point
// ─────────────────────────────────────────────────────────────────────────────

Val Ownership::intoOwned(Val val, llvm::IRBuilder<>& builder) {
    if (!val.isValid()) return val;

    // Already owned: nothing to do. The caller takes over the claim.
    if (val.own == Own::Owned) {
        return val;
    }

    // Borrowed: acquire a fresh claim.
    ResourceKind kind = classifyResourceKind(val.ty);

    switch (kind) {
        case ResourceKind::None: {
            // Bit copy. The "fresh claim" is just the same value.
            // Non-resources have no shared state to duplicate.
            Val result = val;
            result.own = Own::Owned;
            return result;
        }

        case ResourceKind::Refcounted: {
            // Retain the env. The fat pointer is unchanged; only the
            // env's refcount is bumped.
            retain(val.ty, val.v, builder);
            Val result = val;
            result.own = Own::Owned;
            return result;
        }

        case ResourceKind::OwnedBuffer: {
            // Check if this is a static string literal (cap == 0).
            // Static: the data pointer points into a private global; a
            // bit copy is safe and free.
            // Non-static: the buffer is heap-owned; a deep copy is
            // required so the new owner has its own allocation.

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

            // Static path: bit copy.
            builder.SetInsertPoint(staticBlock);
            builder.CreateBr(mergeBlock);

            // Non-static path: deep copy via `__lucid_str_concat(empty, src)`.
            // An empty string as the first argument contributes nothing,
            // and the runtime allocates a fresh buffer for the result.
            //
            // TODO: a proper `__lucid_str_dup` would be cheaper (one
            // allocation, no concatenation loop). Once the runtime adds
            // it, this call becomes `str_dup(src)`.
            builder.SetInsertPoint(copyBlock);
            llvm::StructType* strTy = program.types().stringType();
            llvm::Value* empty = llvm::ConstantAggregateZero::get(strTy);
            llvm::AllocaInst* copySlot = builder.CreateAlloca(
                strTy, nullptr, "str_dup_slot");
            llvm::Function* concatFn =
                program.abi().declareOrGet(RuntimeFn::StrConcat);
            builder.CreateCall(concatFn, {copySlot, empty, val.v});
            llvm::Value* copied =
                builder.CreateLoad(strTy, copySlot, "str_dup_result");
            builder.CreateBr(mergeBlock);

            // Merge: phi picks the right value.
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
            // Call `__copy_<type>`. The generated function takes a pointer
            // to the source and returns a pointer to a freshly allocated
            // aggregate with the same contents, except that resource fields
            // are deep-copied or retained.
            //
            // The emitter's representation of an aggregate value is by
            // value (an SSA value of the struct type), but the copy glue
            // works by pointer. So we spill the source to an alloca, call
            // the glue, and load the result.
            llvm::Function* glue = copyGlueFor(val.ty);
            if (!glue) {
                // Copy glue generation failed. Return the source unchanged
                // — the caller will get an Owned tag on a borrowed value,
                // which is a bug, but at least it doesn't crash.
                Val result = val;
                result.own = Own::Owned;
                return result;
            }

            llvm::AllocaInst* srcSlot = builder.CreateAlloca(
                val.v->getType(), nullptr, "copy_src");
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
        case ResourceKind::Handle:
            // Linear types. Sema rejects copies. If codegen reaches here,
            // it's a Sema bug. Return the value unchanged; the caller
            // will likely produce a wrong result, but the compiler won't
            // crash.
            {
                Val result = val;
                result.own = Own::Owned;
                return result;
            }
    }

    // Unreachable — the switch is exhaustive.
    return val;
}

// ─────────────────────────────────────────────────────────────────────────────
// Transitional free-function wrappers
// ─────────────────────────────────────────────────────────────────────────────
//
// These forward to the `Ownership` object on the caller's `ProgramState`.
// They exist to keep old call sites compiling during the migration. They
// are deleted at the end of Phase 3.

void emitRelease(ValueDeclAST* decl, llvm::Value* value, CodeGenContext& ctx) {
    if (!decl || !decl->type || !value) return;
    if (!ctx.getCurrentFunction()) return;

    // The old `emitRelease` normalized alloca-to-value internally. The
    // new `Ownership::drop` expects a value, not an alloca. To preserve
    // the old behavior for old call sites, load the value if it's an
    // alloca before calling drop.
    llvm::Value* val = value;
    if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
        val = ctx.builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                      "ownership_load");
    }
    ctx.program().ownership().drop(decl->type, val, ctx.builder);
}

void emitRetain(ValueDeclAST* decl, llvm::Value* value, CodeGenContext& ctx) {
    if (!decl || !decl->type || !value) return;
    if (!ctx.getCurrentFunction()) return;

    llvm::Value* val = value;
    if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
        val = ctx.builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                      "ownership_load");
    }
    ctx.program().ownership().retain(decl->type, val, ctx.builder);
}

ResourceKind classifyResource(ValueDeclAST* decl) {
    return decl ? decl->resourceKind : ResourceKind::None;
}

bool ownsResource(ValueDeclAST* decl) {
    return classifyResource(decl) != ResourceKind::None;
}

llvm::Value* maybeCoerceFnToCls(llvm::Value* sourceValue,
                                TypeAST* sourceType,
                                TypeAST* targetType,
                                CodeGenContext& ctx) {
    if (!sourceValue || !sourceType || !targetType) return sourceValue;
    if (!sourceType->isa<FuncTypeAST>() || !targetType->isa<FuncTypeAST>()) {
        return sourceValue;
    }

    FuncTypeAST* srcFunc = sourceType->as<FuncTypeAST>();
    FuncTypeAST* tgtFunc = targetType->as<FuncTypeAST>();

    if (srcFunc->shape != FuncShape::Fn || tgtFunc->shape != FuncShape::Cls) {
        return sourceValue;
    }

    llvm::StructType* closureType = ctx.program().types().closureType();
    llvm::Value* wrapped = llvm::UndefValue::get(closureType);
    wrapped = ctx.builder.CreateInsertValue(
        wrapped, sourceValue, 0, "fn_to_cls_func");
    wrapped = ctx.builder.CreateInsertValue(
        wrapped,
        llvm::ConstantPointerNull::get(
            llvm::PointerType::get(ctx.llvmCtx, 0)),
        1, "fn_to_cls_env");
    return wrapped;
}

} // namespace codegen