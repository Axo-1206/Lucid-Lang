/// @file support/CodeGenPanic.cpp
/// @brief Implementation of runtime panic and null check handling.

#include "CodeGenPanic.hpp"
#include "RuntimeError.hpp"
#include "../CodeGenType.hpp"
#include "debug/DebugUtils.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

namespace codegen {

// ─── Internal Helper ──────────────────────────────────────────────────────

static void emitPanicInternal(const std::string& message, CodeGenContext& ctx) {
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

// ─── Public API ────────────────────────────────────────────────────────────

void emitPanic(RuntimeErrorKind kind, CodeGenContext& ctx) {
    emitPanicInternal(getRuntimeErrorMessage(kind), ctx);
}

void emitPanic(const std::string& message, CodeGenContext& ctx) {
    emitPanicInternal(message, ctx);
}

llvm::Value* emitNullCheck(llvm::Value* ptr, CodeGenContext& ctx) {
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
    emitPanic(RuntimeErrorKind::NullPointerDereference, ctx);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(ptrType, 2, "null_check_result");
    phi->addIncoming(ptr, passBlock);
    phi->addIncoming(llvm::Constant::getNullValue(ptrType), failBlock);

    return phi;
}

llvm::Value* emitBoundsCheck(
    llvm::Value* index,
    llvm::Value* length,
    CodeGenContext& ctx
) {
    if (!index || !length) return index;

    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.llvmCtx);
    if (index->getType() != i64Ty) {
        index = ctx.builder.CreateIntCast(index, i64Ty, true, "idx_cast");
    }
    if (length->getType() != i64Ty) {
        length = ctx.builder.CreateIntCast(length, i64Ty, true, "len_cast");
    }

    llvm::Value* idxNeg = ctx.builder.CreateICmpSLT(
        index,
        llvm::ConstantInt::get(i64Ty, 0),
        "idx_neg"
    );
    llvm::Value* idxGE = ctx.builder.CreateICmpSGE(
        index,
        length,
        "idx_ge_len"
    );
    llvm::Value* outOfBounds = ctx.builder.CreateOr(idxNeg, idxGE, "idx_oob");

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* passBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "bounds_ok", func);
    llvm::BasicBlock* failBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "bounds_fail", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "bounds_merge", func);

    ctx.builder.CreateCondBr(outOfBounds, failBlock, passBlock);

    ctx.builder.SetInsertPoint(passBlock);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(failBlock);
    emitPanic(RuntimeErrorKind::ArrayIndexOutOfBounds, ctx);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(i64Ty, 2, "bounds_result");
    phi->addIncoming(index, passBlock);
    phi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), failBlock);

    return phi;
}

std::pair<llvm::Value*, llvm::Value*> emitSliceBoundsCheck(
    llvm::Value* start,
    llvm::Value* end,
    llvm::Value* length,
    CodeGenContext& ctx
) {
    if (!start || !end || !length) return {start, end};

    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.llvmCtx);
    if (start->getType() != i64Ty) {
        start = ctx.builder.CreateIntCast(start, i64Ty, true, "start_cast");
    }
    if (end->getType() != i64Ty) {
        end = ctx.builder.CreateIntCast(end, i64Ty, true, "end_cast");
    }
    if (length->getType() != i64Ty) {
        length = ctx.builder.CreateIntCast(length, i64Ty, true, "len_cast");
    }

    llvm::Value* startNeg = ctx.builder.CreateICmpSLT(start, llvm::ConstantInt::get(i64Ty, 0), "start_neg");
    llvm::Value* startGTEnd = ctx.builder.CreateICmpSGT(start, end, "start_gt_end");
    llvm::Value* endGTLen = ctx.builder.CreateICmpSGT(end, length, "end_gt_len");

    llvm::Value* outOfBounds = ctx.builder.CreateOr(
        startNeg,
        ctx.builder.CreateOr(startGTEnd, endGTLen, "slice_oob_or"),
        "slice_oob"
    );

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* passBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "slice_bounds_ok", func);
    llvm::BasicBlock* failBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "slice_bounds_fail", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "slice_bounds_merge", func);

    ctx.builder.CreateCondBr(outOfBounds, failBlock, passBlock);

    ctx.builder.SetInsertPoint(passBlock);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(failBlock);
    emitPanic(RuntimeErrorKind::SliceBoundsOutOfRange, ctx);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* startPhi = ctx.builder.CreatePHI(i64Ty, 2, "start_result");
    startPhi->addIncoming(start, passBlock);
    startPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), failBlock);

    llvm::PHINode* endPhi = ctx.builder.CreatePHI(i64Ty, 2, "end_result");
    endPhi->addIncoming(end, passBlock);
    endPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), failBlock);

    return {startPhi, endPhi};
}

llvm::Value* emitZeroCheck(llvm::Value* divisor, RuntimeErrorKind kind, CodeGenContext& ctx) {
    if (!divisor) return nullptr;

    llvm::Value* isZero = ctx.builder.CreateICmpEQ(
        divisor,
        llvm::ConstantInt::get(divisor->getType(), 0),
        "divisor_is_zero"
    );

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* passBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "zero_check_ok", func);
    llvm::BasicBlock* failBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "zero_check_fail", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "zero_check_merge", func);

    ctx.builder.CreateCondBr(isZero, failBlock, passBlock);

    ctx.builder.SetInsertPoint(passBlock);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(failBlock);
    emitPanic(kind, ctx);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(divisor->getType(), 2, "zero_check_result");
    phi->addIncoming(divisor, passBlock);
    phi->addIncoming(llvm::ConstantInt::get(divisor->getType(), 0), failBlock);

    return phi;
}

} // namespace codegen