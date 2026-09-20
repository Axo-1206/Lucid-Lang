/// @file codegen/context/CodeGenContext.cpp
/// @brief Implementation of the transitional aggregator.
///
/// Most of `CodeGenContext` is inline forwarders in the header. This file
/// contains the four methods with real logic:
///
///   - `setCurrentFunction` / `clearCurrentFunction` — construct and destroy
///     the `FunctionState`
///   - `emitUnwindTo` — walk the scope stack and emit cleanup
///   - `createStringLiteral` — lower a string literal to an `llvm::Value*`
///   - `reassign` — transitional; delegates to the eventual ownership API
///
/// `getLLVMIntrinsicDecl` is also here because it calls into LLVM's
/// intrinsic machinery, which shouldn't be in the header.

#include "CodeGenContext.hpp"

#include "codegen/ownership/CodeGenOwnership.hpp"
#include "codegen/support/CodeGenPanic.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Function Body Entry / Exit
// ─────────────────────────────────────────────────────────────────────────────

void CodeGenContext::setCurrentFunction(llvm::Function* fn,
                                         TypeAST* declaredReturnType) {
    assert(fn && "setCurrentFunction requires a non-null llvm::Function");

    // Constructing a `FunctionState`:
    //   - captures the enclosing function state
    //   - installs `fn` as the current function
    //   - clears the scope and loop stacks for the new function
    //   - saves the builder's insertion point
    //
    // The caller is expected to subsequently create the entry block and
    // call `builder().SetInsertPoint(entry)`.
    //
    // Nested calls (a closure body inside a function body) construct a
    // nested `FunctionState`. The enclosing state is restored when the
    // nested `FunctionState` is destroyed.
    func = std::make_unique<FunctionState>(prog, fn, declaredReturnType);
}

void CodeGenContext::clearCurrentFunction() {
    func.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// Cleanup and Unwind
// ─────────────────────────────────────────────────────────────────────────────

void CodeGenContext::emitUnwindTo(size_t targetDepth) {
    if (!func) return;

    // Walk the scope stack from the innermost scope to (but not including)
    // `targetDepth`, emitting cleanup for each scope. Non-destructive: the
    // scope stack is not popped, and the trackers are not mutated. Only
    // the structurally-paired `popLiveScope` call may remove a scope from
    // the stack.
    //
    // The cleanup logic itself (walking the alive set, dispatching to
    // `emitRelease`) lives in `emitCleanupForScope`, defined below as a
    // free function so `emitUnwindTo` can call it without going through
    // the shim's method surface. Task 4 will move this logic to
    // `Ownership`.

    auto& scopes = func->scopeStack();
    if (targetDepth >= scopes.size()) return;

    for (size_t i = scopes.size(); i > targetDepth; --i) {
        Scope& scope = scopes[i - 1];
        emitCleanupForScope(scope, prog, *this);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// String Literal Lowering
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* CodeGenContext::createStringLiteral(const std::string& str) {
    llvm::LLVMContext& llvmCtx = prog.llvmContext();
    llvm::IRBuilder<>& builder = prog.builder();

    // ─── Global constant for the bytes ────────────────────────────────────
    llvm::Constant* strConst =
        llvm::ConstantDataArray::getString(llvmCtx, str);
    llvm::GlobalVariable* global = new llvm::GlobalVariable(
        prog.module(),
        strConst->getType(),
        /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage,
        strConst);

    // ─── Build the `lucid.String` value ───────────────────────────────────
    // `{ ptr data, i64 len, i64 cap }`. A literal's `cap` is set to 0 to
    // signal "static data — do not free". This is the sentinel the
    // ownership layer checks before freeing a string's data pointer.
    llvm::StructType* strType = prog.types().stringType();
    llvm::Type* i64 = llvm::Type::getInt64Ty(llvmCtx);
    llvm::Type* i8Ptr = llvm::PointerType::get(llvmCtx, 0);

    llvm::Value* ptr = builder.CreateBitCast(global, i8Ptr);
    llvm::Value* len = llvm::ConstantInt::get(i64, str.length());
    llvm::Value* cap = llvm::ConstantInt::get(i64, 0);  // static

    llvm::Value* result = llvm::UndefValue::get(strType);
    result = builder.CreateInsertValue(result, ptr, 0);
    result = builder.CreateInsertValue(result, len, 1);
    result = builder.CreateInsertValue(result, cap, 2);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Intrinsics
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* CodeGenContext::getLLVMIntrinsicDecl(
    llvm::Intrinsic::ID id,
    llvm::ArrayRef<llvm::Type*> argTypes) {
    return llvm::Intrinsic::getDeclaration(&prog.module(), id, argTypes);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reassignment
// ─────────────────────────────────────────────────────────────────────────────

void CodeGenContext::reassign(ValueDeclAST* decl,
                               llvm::Value* oldValue,
                               llvm::Value* newValue) {
    if (!decl || !oldValue || !newValue) return;
    if (!func) return;

    if (!isAlive(decl)) return;  // Nothing to clean up

    TypeAST* type = decl->type;
    if (!type) return;

    // ─── Reject linear types ──────────────────────────────────────────────
    // Future<T> and Thread<T> cannot be reassigned while pending/running.
    // This is a semantic check, not a resource-kind check, so it lives
    // here rather than in `classifyResourceKind`. Sema should have already
    // rejected this, but the assertion is a cheap backstop.
    if (type->isa<FutureTypeAST>() || type->isa<ThreadTypeAST>()) {
        prog.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, decl->loc,
                                  "internal error: linear type cannot be "
                                  "reassigned while pending/running");
        return;
    }

    // ─── Release the old resource ─────────────────────────────────────────
    // The binding stays alive; only the old value's claim is dropped.
    // `emitRelease` is the transitional name for `Ownership::drop` — Task 4
    // renames and re-signatures it.
    //
    // The decision to retain the *new* value (Rule 1 vs Rule 2 in the
    // ownership model) is the caller's responsibility, because only the
    // caller knows whether the RHS was a fresh literal or an existing
    // binding.
    emitRelease(decl, oldValue, *this);
    (void)newValue;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cleanup Helper — transitional
// ─────────────────────────────────────────────────────────────────────────────
//
// This is the same logic that used to be `CodeGenContext::emitCleanupForTracker`
// in the old code, adapted to the new `Scope` type and the new `ProgramState`.
// Task 4 moves it to `Ownership` as `Ownership::dropScope`.

void emitCleanupForScope(Scope& scope,
                         ProgramState& prog,
                         CodeGenContext& ctx) {
    if (!ctx.getCurrentFunction()) return;

    llvm::IRBuilder<>& builder = prog.builder();

    // ─── Phase 1: user #scope_exit callbacks (LIFO) ───────────────────────
    if (scope.block) {
        for (size_t i = scope.block->scopeExits.size(); i > 0; --i) {
            const ScopeExitRegistration* reg =
                scope.block->scopeExits[i - 1];
            emitScopeExitCallback(reg, ctx);
        }
    }

    // ─── Phase 2: implicit cleanup ────────────────────────────────────────
    // For each still-alive binding, load its current value and hand it to
    // `emitRelease`. The alive set is the source of truth for "this frame
    // owns a claim"; consumed bindings are not in it.
    std::vector<ValueDeclAST*> declarations(
        scope.alive.begin(), scope.alive.end());

    auto loadIfAlloca = [&](llvm::Value* val) -> llvm::Value* {
        if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(val)) {
            return builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                       "cleanup_load");
        }
        return val;
    };

    for (ValueDeclAST* decl : declarations) {
        llvm::Value* binding = ctx.lookupValue(decl);
        if (!binding || !decl->type) continue;

        llvm::Value* value = loadIfAlloca(binding);
        emitRelease(decl, value, ctx);
    }
}

} // namespace codegen