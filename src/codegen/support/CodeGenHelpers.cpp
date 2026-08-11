/// @file CodeGenHelpers.cpp
/// @brief Implementation of code generation helper functions.

#include "CodeGenHelpers.hpp"
#include "../CodeGenType.hpp"  // ← Add this include
#include "debug/DebugUtils.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

namespace codegen {

// ─── Alloca Creation ──────────────────────────────────────────────────────

llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        return nullptr;
    }

    llvm::BasicBlock* entryBlock = &func->getEntryBlock();
    llvm::IRBuilder<> builder(ctx.llvmCtx);
    builder.SetInsertPoint(entryBlock, entryBlock->getFirstInsertionPt());

    return builder.CreateAlloca(type, nullptr, name);
}

llvm::BasicBlock* createBlock(
    const std::string& name,
    CodeGenContext& ctx
) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        return nullptr;
    }

    return llvm::BasicBlock::Create(ctx.llvmCtx, name, func);
}

// ─── Load Helpers ─────────────────────────────────────────────────────────

llvm::Value* loadIfNeeded(
    llvm::Value* value,
    llvm::Type* elemType,
    CodeGenContext& ctx
) {
    if (!value || !elemType) return value;

    if (value->getType()->isPointerTy()) {
        return ctx.builder.CreateLoad(elemType, value);
    }

    return value;
}

llvm::Value* loadIfNeeded(
    llvm::Value* value,
    bool isLValue,
    CodeGenContext& ctx
) {
    if (!value || !isLValue) return value;

    // With opaque pointers, we can't get the element type from the pointer.
    // Return the pointer and let the caller handle it.
    return value;
}

// ─── Panic ─────────────────────────────────────────────────────────────────

void emitPanic(
    const std::string& message,
    CodeGenContext& ctx
) {
    // ─── Get the panic function from the runtime ──────────────────────────
    llvm::Function* panicFunc = ctx.getRuntimeFunction("__lucid_panic");
    if (!panicFunc) {
        // Declare the panic function
        llvm::FunctionType* panicType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {llvm::PointerType::get(ctx.llvmCtx, 0)},
            false
        );
        panicFunc = llvm::Function::Create(
            panicType,
            llvm::Function::ExternalLinkage,
            "__lucid_panic",
            ctx.module
        );
        ctx.setRuntimeFunction("__lucid_panic", panicFunc);
    }

    // ─── Create a global string for the message ──────────────────────────
    llvm::Constant* msgConst = llvm::ConstantDataArray::getString(
        ctx.llvmCtx,
        message,
        true
    );

    llvm::GlobalVariable* msgGlobal = new llvm::GlobalVariable(
        *ctx.module,
        msgConst->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        msgConst,
        "panic_msg"
    );

    // ─── Get a pointer to the string ──────────────────────────────────────
    llvm::Value* msgPtr = ctx.builder.CreatePointerCast(
        msgGlobal,
        llvm::PointerType::get(ctx.llvmCtx, 0)
    );

    // ─── Call panic ───────────────────────────────────────────────────────
    ctx.builder.CreateCall(panicFunc, {msgPtr});

    // ─── Panic should not return - emit unreachable ──────────────────────
    ctx.builder.CreateUnreachable();
}

// ─── Type Helpers ─────────────────────────────────────────────────────────

llvm::Type* getDeclType(
    const ValueDeclAST* decl,
    CodeGenContext& ctx
) {
    if (!decl) return nullptr;
    // Now getType is visible (from CodeGenType.hpp)
    return getType(ctx, decl->type);
}

// ─── Name Helpers ─────────────────────────────────────────────────────────

std::string getMangledName(
    const FuncDeclAST* decl,
    CodeGenContext& ctx
) {
    if (!decl) return "";

    // Simple mangling: just the name for now
    // TODO: Implement proper name mangling for generics and overloads
    return ctx.pool.lookup(decl->name);
}

} // namespace codegen