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
// ownsResource
// ─────────────────────────────────────────────────────────────────────────────

bool ownsResource(ValueDeclAST* decl) {
    if (!decl || !decl->type) return false;

    TypeAST* type = decl->type;

    // ─── Function-typed bindings ─────────────────────────────────────────
    // A FuncDeclAST with hasClosure owns its env. A FuncDeclAST without
    // hasClosure is a bare function pointer and owns nothing. A VarDeclAST
    // never holds a FuncTypeAST in Lucid (the parser's looksLikeFuncDecl
    // guarantees it), so we don't need a separate VarDeclAST branch.
    if (type->isa<FuncTypeAST>()) {
        if (decl->isa<FuncDeclAST>()) {
            return decl->as<FuncDeclAST>()->hasClosure;
        }
        return false;
    }

    // ─── Strings ─────────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        PrimitiveTypeAST* prim = type->as<PrimitiveTypeAST>();
        return prim->primitiveKind == PrimitiveKind::String;
    }

    // ─── Dynamic arrays ──────────────────────────────────────────────────
    // Slice ([_]T) and fixed ([N]T) arrays are non-owning; only the
    // dynamic kind ([*]T) has a heap buffer to release.
    if (type->isa<ArrayTypeAST>()) {
        return type->as<ArrayTypeAST>()->isDynamic();
    }

    // ─── Named types (structs) ───────────────────────────────────────────
    // Phase 5 will add recursive structOwnsResources(StructDeclAST*) here.
    // Until then, a struct-typed binding is treated as owning nothing, which
    // matches the current behavior of emitCleanupForTracker and reassign.
    //
    // TODO(Phase 5): replace this with structOwnsResources once it exists.
    if (type->isa<NamedTypeAST>()) {
        return false;
    }

    // ─── TaggedSlot-wrapped resources (erased path) ──────────────────────
    // Phase 4 will extend this. Until then, an erased binding is treated as
    // owning nothing, which matches the current behavior.
    //
    // TODO(Phase 4): unwrap the TaggedSlot and recurse on the payload type.
    if (type->isa<NullableTypeAST>() ||
        type->isa<FallibleTypeAST>() ||
        type->isa<CombinedTypeAST>()) {
        return false;
    }

    // ─── Everything else ─────────────────────────────────────────────────
    // Primitives (int, bool, float, char), enums, raw pointers, references,
    // function types without hasClosure, fixed arrays, slices. None of
    // these own a heap resource.
    return false;
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

    TypeAST* type = decl->type;

    // ─── Closures ────────────────────────────────────────────────────────
    if (type->isa<FuncTypeAST>()) {
        // Only a FuncDeclAST with hasClosure owns a fat pointer. A bare
        // function pointer has no env to release.
        if (!decl->isa<FuncDeclAST>()) return;
        if (!decl->as<FuncDeclAST>()->hasClosure) return;

        if (!isClosureShaped(value)) {
            // The AST says this is a capturing function, but the LLVM
            // value isn't a fat pointer. That's a CodeGen invariant
            // violation — every capturing function's value must be
            // closure-shaped by the time it's marked alive.
            //
            // Not asserting here because Phase 1 may produce this state
            // transiently during the transition. Phase 8 tightens it to
            // an assert once all call sites are updated.
            return;
        }

        llvm::Value* envPtr = ctx.builder.CreateExtractValue(
            value, 1, "closure_env_to_release");

        llvm::Function* releaseFn = ctx.getRuntimeFn(RuntimeFn::ReleaseEnv);
        emitNullCheckedRelease(envPtr, releaseFn, ctx, "release_env");
        return;
    }

    // ─── Strings ─────────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        PrimitiveTypeAST* prim = type->as<PrimitiveTypeAST>();
        if (prim->primitiveKind != PrimitiveKind::String) return;

        if (!isStringOrArrayShaped(value)) return;

        llvm::Value* dataPtr = ctx.builder.CreateExtractValue(
            value, 0, "string_data_to_release");

        // Static string literals are global variables, not heap allocations.
        // Freeing them would corrupt the allocator.
        if (isStaticStringData(dataPtr)) return;

        llvm::Function* freeFn = ctx.getRuntimeFn(RuntimeFn::Free);
        emitNullCheckedRelease(dataPtr, freeFn, ctx, "release_string");
        return;
    }

    // ─── Dynamic arrays ──────────────────────────────────────────────────
    if (type->isa<ArrayTypeAST>()) {
        if (!type->as<ArrayTypeAST>()->isDynamic()) return;

        if (!isStringOrArrayShaped(value)) return;

        llvm::Value* dataPtr = ctx.builder.CreateExtractValue(
            value, 0, "array_data_to_release");

        llvm::Function* freeFn = ctx.getRuntimeFn(RuntimeFn::Free);
        emitNullCheckedRelease(dataPtr, freeFn, ctx, "release_array");
        return;
    }

    // ─── Structs with resource fields ────────────────────────────────────
    // TODO(Phase 5): walk the struct's fields, GEP to each resource-owning
    // one, load its value, and recurse into emitRelease.
    //
    // ─── TaggedSlot-wrapped resources ────────────────────────────────────
    // TODO(Phase 4): extract the payload pointer, load it as the concrete
    // resource type, recurse into emitRelease.
    //
    // Nothing to do for other types.
}

// ─────────────────────────────────────────────────────────────────────────────
// emitRetain
// ─────────────────────────────────────────────────────────────────────────────

void emitRetain(ValueDeclAST* decl, llvm::Value* value, CodeGenContext& ctx) {
    if (!decl || !decl->type || !value) return;
    if (!ctx.getCurrentFunction()) return;

    value = loadIfAlloca(value, ctx);
    if (!value) return;

    TypeAST* type = decl->type;

    // ─── Closures ────────────────────────────────────────────────────────
    if (type->isa<FuncTypeAST>()) {
        if (!decl->isa<FuncDeclAST>()) return;
        if (!decl->as<FuncDeclAST>()->hasClosure) return;

        if (!isClosureShaped(value)) return;

        llvm::Value* envPtr = ctx.builder.CreateExtractValue(
            value, 1, "closure_env_to_retain");

        llvm::Function* retainFn = ctx.getRuntimeFn(RuntimeFn::RetainEnv);
        emitNullCheckedRelease(envPtr, retainFn, ctx, "retain_env");
        return;
    }

    // ─── Strings, dynamic arrays ─────────────────────────────────────────
    // No retain needed. When a string or array is copied into a new binding,
    // the copy is a deep copy (a fresh allocation), not a refcount increment.
    // The new binding owns its own independent buffer and will release it
    // through its own emitRelease. No shared state, no retain call.
    //
    // This branch is documented rather than omitted so future readers
    // understand why there's no code here, not just that there isn't.

    // ─── Structs with resource fields ────────────────────────────────────
    // TODO(Phase 5): copy the struct value, walk its fields, and for each
    // resource-owning field whose value is refcounted (closures), recurse
    // into emitRetain. Strings and arrays inside structs are deep-copied
    // when the struct is copied, so no retain is needed for those.
    //
    // ─── TaggedSlot-wrapped resources ────────────────────────────────────
    // TODO(Phase 4): unwrap and recurse.
}

} // namespace codegen