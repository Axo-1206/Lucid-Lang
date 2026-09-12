/// @file support/CodeGenOwnership.cpp
/// @brief Implementation of the unified ownership API.
///
/// See CodeGenOwnership.hpp for the design rationale. This file implements
/// the three functions in dispatch order: ownsResource, emitRelease,
/// emitRetain.

#include "CodeGenOwnership.hpp"
#include "../runtime/RuntimeFunctionRegistry.hpp"
#include "../types/CodeGenType.hpp"
#include "../types/LLVMTypeHelpers.hpp"

#include "core/ast/TypeAST.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// @brief Load a value out of an alloca if the input is an alloca, else
///        return the input unchanged.
///
/// emitRelease / emitRetain are specified to take a value, not an alloca.
/// But call sites in the tracker (emitCleanupForTracker) hold the alloca,
/// because that's what ctx.storeValue stores. Rather than force every call
/// site to write its own load, this helper normalizes the input.
///
/// The distinction matters at the IR level: an alloca of type { ptr, ptr }
/// is an address, not a closure. extractvalue on it would produce a wrong
/// result. So load first, then extract.
static llvm::Value* loadIfAlloca(llvm::Value* value, CodeGenContext& ctx) {
    if (!value) return nullptr;
    if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
        return ctx.builder.CreateLoad(
            alloca->getAllocatedType(), alloca, "ownership_load");
    }
    if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(value)) {
        return ctx.builder.CreateLoad(
            global->getValueType(), global, "ownership_load_global");
    }
    return value;
}

/// @brief Check whether an LLVM value is a closure fat pointer.
///
/// The closure type is a 2-element struct { ptr, ptr }: function pointer
/// followed by environment pointer. This is the shape lowerClosure builds
/// and emitClosureCall / emitCallableCall consume.
static bool isClosureShaped(llvm::Value* value) {
    if (!value) return false;
    llvm::Type* ty = value->getType();
    return ty->isStructTy() && ty->getStructNumElements() == 2;
}

/// @brief Check whether an LLVM value is the shape of a string or array.
///
/// Both lower to the same 3-field struct { ptr, i64, i64 }: data pointer,
/// length, capacity. The distinction between string and array is purely
/// at the AST level, not the LLVM level.
static bool isStringOrArrayShaped(llvm::Value* value) {
    if (!value) return false;
    llvm::Type* ty = value->getType();
    return ty->isStructTy() && ty->getStructNumElements() == 3;
}

/// @brief Emit the null-checked release pattern:
///
///     %is_null = icmp eq ptr %ptr, null
///     br %is_null, %skip, %release
///   release:
///     call void @releaseFn(%ptr)
///     br %skip
///   skip:
///
/// On return, ctx.builder's insertion point is at the start of the skip
/// block, ready for the caller's next instruction.
static void emitNullCheckedRelease(
    llvm::Value* ptr,
    llvm::Function* releaseFn,
    CodeGenContext& ctx,
    const llvm::Twine& namePrefix
) {
    llvm::Function* func = ctx.getCurrentFunction();
    assert(func && "emitNullCheckedRelease: no current function");

    llvm::BasicBlock* releaseBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx, namePrefix + ".release", func);
    llvm::BasicBlock* skipBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx, namePrefix + ".skip", func);

    llvm::Value* isNull = ctx.builder.CreateIsNull(ptr, namePrefix + ".is_null");
    ctx.builder.CreateCondBr(isNull, skipBlock, releaseBlock);

    ctx.builder.SetInsertPoint(releaseBlock);
    ctx.builder.CreateCall(releaseFn, {ptr});
    ctx.builder.CreateBr(skipBlock);

    ctx.builder.SetInsertPoint(skipBlock);
}

/// @brief Check whether a value is statically known to be a global string
///        literal (and therefore must NOT be freed).
///
/// String literals are lowered as GlobalVariable pointers with private
/// linkage; they are not heap-allocated, so freeing them would corrupt the
/// allocator. The existing emitCleanupForTracker and reassign both do this
/// check; this helper centralizes it.
static bool isStaticStringData(llvm::Value* dataPtr) {
    if (!dataPtr) return false;
    if (auto* constant = llvm::dyn_cast<llvm::Constant>(dataPtr)) {
        return llvm::isa<llvm::GlobalVariable>(constant);
    }
    return false;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// classifyResource
// ─────────────────────────────────────────────────────────────────────────────

ResourceKind classifyResource(ValueDeclAST* decl) {
    if (!decl || !decl->type) return ResourceKind::None;

    TypeAST* type = decl->type;

    // ─── Function-typed bindings ─────────────────────────────────────────
    // A FuncDeclAST with hasClosure owns its env (refcounted). A FuncDeclAST
    // without hasClosure is a bare function pointer and owns nothing. A
    // VarDeclAST never holds a FuncTypeAST in Lucid (the parser's
    // looksLikeFuncDecl guarantees it), so no separate VarDeclAST branch.
    if (type->isa<FuncTypeAST>()) {
        if (decl->isa<FuncDeclAST>()) {
            FuncDeclAST* func = decl->as<FuncDeclAST>();
            bool capturing = func->init
                && func->init->isa<AnonFuncExprAST>()
                && func->init->as<AnonFuncExprAST>()->hasClosure;
            return capturing ? ResourceKind::Refcounted : ResourceKind::None;
        }
        return ResourceKind::None;
    }

    // ─── Strings ─────────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        PrimitiveTypeAST* prim = type->as<PrimitiveTypeAST>();
        return prim->primitiveKind == PrimitiveKind::String
            ? ResourceKind::OwnedBuffer : ResourceKind::None;
    }

    // ─── Dynamic arrays ──────────────────────────────────────────────────
    if (type->isa<ArrayTypeAST>()) {
        return type->as<ArrayTypeAST>()->isDynamic()
            ? ResourceKind::OwnedBuffer : ResourceKind::None;
    }

    // ─── Named types (structs) ───────────────────────────────────────────
    // TODO(Phase 5): recurse into fields once structOwnsResources exists.
    if (type->isa<NamedTypeAST>()) {
        return ResourceKind::None;
    }

    // ─── TaggedSlot-wrapped resources (erased path) ──────────────────────
    // TODO(Phase 4): unwrap the payload type and recurse.
    if (type->isa<NullableTypeAST>() ||
        type->isa<FallibleTypeAST>() ||
        type->isa<CombinedTypeAST>()) {
        return ResourceKind::None;
    }

    return ResourceKind::None;
}

// ─────────────────────────────────────────────────────────────────────────────
// ownsResource
// ─────────────────────────────────────────────────────────────────────────────

bool ownsResource(ValueDeclAST* decl) {
    return classifyResource(decl) != ResourceKind::None;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitRelease
// ─────────────────────────────────────────────────────────────────────────────

void emitRelease(ValueDeclAST* decl, llvm::Value* value, CodeGenContext& ctx) {
    if (!decl || !decl->type || !value) return;
    if (!ctx.getCurrentFunction()) return;

    // Normalize alloca → value.
    value = loadIfAlloca(value, ctx);
    if (!value) return;

    switch (classifyResource(decl)) {
        case ResourceKind::None:
            return;

        case ResourceKind::Refcounted: {
            // Closure env: extract field 1, null-checked release.
            if (!isClosureShaped(value)) {
                // AST says capturing function; LLVM value isn't a fat
                // pointer. CodeGen invariant violation. Not asserting yet
                // (see the original comment for the transition rationale).
                return;
            }
            llvm::Value* envPtr = ctx.builder.CreateExtractValue(
                value, 1, "closure_env_to_release");
            llvm::Function* releaseFn = ctx.getRuntimeFn(RuntimeFn::ReleaseEnv);
            emitNullCheckedRelease(envPtr, releaseFn, ctx, "release_env");
            return;
        }

        case ResourceKind::OwnedBuffer: {
            // String or dynamic array: field 0 is the data pointer.
            if (!isStringOrArrayShaped(value)) return;
            llvm::Value* dataPtr = ctx.builder.CreateExtractValue(
                value, 0, "buffer_data_to_release");

            // Static string literals are globals, not heap allocations.
            // Freeing them would corrupt the allocator.
            if (isStaticStringData(dataPtr)) return;

            llvm::Function* freeFn = ctx.getRuntimeFn(RuntimeFn::Free);
            emitNullCheckedRelease(dataPtr, freeFn, ctx, "release_buffer");
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitRetain
// ─────────────────────────────────────────────────────────────────────────────

void emitRetain(ValueDeclAST* decl, llvm::Value* value, CodeGenContext& ctx) {
    if (!decl || !decl->type || !value) return;
    if (!ctx.getCurrentFunction()) return;

    value = loadIfAlloca(value, ctx);
    if (!value) return;

    switch (classifyResource(decl)) {
        case ResourceKind::None:
            return;

        case ResourceKind::Refcounted: {
            if (!isClosureShaped(value)) return;
            llvm::Value* envPtr = ctx.builder.CreateExtractValue(
                value, 1, "closure_env_to_retain");
            llvm::Function* retainFn = ctx.getRuntimeFn(RuntimeFn::RetainEnv);
            emitNullCheckedRelease(envPtr, retainFn, ctx, "retain_env");
            return;
        }

        case ResourceKind::OwnedBuffer:
            // Deep-copy semantics: the destination already owns a fresh
            // allocation. No retain needed. Documented, not omitted.
            return;
    }
}

} // namespace codegen