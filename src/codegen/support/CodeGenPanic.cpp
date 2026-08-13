/// @file support/CodeGenPanic.cpp
/// @brief Implementation of runtime panic and null check handling.

#include "CodeGenPanic.hpp"
#include "../CodeGenType.hpp"
#include "debug/DebugUtils.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

namespace codegen {

// ─── Panic ─────────────────────────────────────────────────────────────────

void emitPanic(
    const std::string& message,
    CodeGenContext& ctx
) {
    llvm::Function* panicFunc = ctx.getRuntimeFunction("__lucid_panic");
    if (!panicFunc) {
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

    llvm::Value* msgPtr = ctx.builder.CreatePointerCast(
        msgGlobal,
        llvm::PointerType::get(ctx.llvmCtx, 0)
    );

    ctx.builder.CreateCall(panicFunc, {msgPtr});
    ctx.builder.CreateUnreachable();
}

llvm::Value* emitNullCheck(
    llvm::Value* ptr,
    const std::string& message,
    CodeGenContext& ctx
) {
    if (!ptr) return nullptr;

    llvm::Type* ptrType = ptr->getType();
    if (!ptrType->isPointerTy()) return ptr;

    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) return ptr;

    llvm::Value* isNull = ctx.builder.CreateICmpEQ(
        ptr,
        llvm::Constant::getNullValue(ptrType),
        "ptr_is_null"
    );

    llvm::BasicBlock* checkBlock = ctx.builder.GetInsertBlock();
    llvm::BasicBlock* passBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "null_check_pass",
        func
    );
    llvm::BasicBlock* failBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "null_check_fail",
        func
    );
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "null_check_merge",
        func
    );

    ctx.builder.CreateCondBr(isNull, failBlock, passBlock);

    ctx.builder.SetInsertPoint(passBlock);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(failBlock);
    emitPanic(message, ctx);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(ptrType, 2, "null_check_result");
    phi->addIncoming(ptr, passBlock);
    phi->addIncoming(llvm::Constant::getNullValue(ptrType), failBlock);

    return phi;
}

} // namespace codegen