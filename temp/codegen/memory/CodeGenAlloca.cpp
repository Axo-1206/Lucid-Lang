/// @file support/CodeGenAlloca.cpp
/// @brief Implementation of memory allocation and basic block management helpers.

#include "CodeGenAlloca.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>

namespace codegen {

// ─── Alloca Creation ──────────────────────────────────────────────────────

llvm::AllocaInst* createAlloca(const std::string& name, llvm::Type* type, CodeGenContext& ctx) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        return nullptr;
    }

    llvm::BasicBlock* entryBlock = &func->getEntryBlock();
    llvm::IRBuilder<> builder(ctx.llvmCtx);
    builder.SetInsertPoint(entryBlock, entryBlock->getFirstInsertionPt());

    return builder.CreateAlloca(type, nullptr, name);
}

llvm::BasicBlock* createBlock(const std::string& name, CodeGenContext& ctx) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        return nullptr;
    }

    return llvm::BasicBlock::Create(ctx.llvmCtx, name, func);
}

// ─── Load Helpers ─────────────────────────────────────────────────────────

llvm::Value* loadIfNeeded(llvm::Value* value, llvm::Type* elemType, CodeGenContext& ctx) {
    if (!value || !elemType) return value;

    if (value->getType()->isPointerTy()) {
        return ctx.builder.CreateLoad(elemType, value);
    }

    return value;
}

} // namespace codegen