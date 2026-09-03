/// @file support/CodeGenPanic.cpp
/// @brief Implementation of panic and bounds checking utilities.

#include "CodeGenPanic.hpp"
#include "../runtime/RuntimeError.hpp"
#include "../types/LLVMTypeHelpers.hpp"
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/IRBuilder.h>

namespace codegen {

// ─── Helper: Build panic message with file name from context ──────────────

std::string buildPanicMessage(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const SourceLocation& loc,
    const std::string& additionalMessage
) {
    std::string baseMsg = additionalMessage.empty() 
        ? getRuntimeErrorMessage(kind) 
        : additionalMessage;

    if (loc.isKnown()) {
        // ─── Get the file name from CodeGenContext ─────────────────────────
        // This is set by CodeGen when processing each module.
        std::string fileName;
        if (ctx.currentFile.isValid()) {
            fileName = ctx.pool.lookup(ctx.currentFile);
        } else {
            fileName = "<unknown file>";
        }
        return fileName + ":" + loc.toString() + ": " + baseMsg;
    }
    return baseMsg;
}

// ─── emitPanic Overloads ────────────────────────────────────────────────────

/// @brief Core implementation - all other overloads delegate here.
static void emitPanicImpl(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const std::string& message,
    const SourceLocation& loc
) {
    llvm::Function* panicFn = ctx.getRuntimeFn(RuntimeFn::Panic);
    if (!panicFn) {
        ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, loc,
                                "panic function not found");
        return;
    }

    std::string fullMsg = buildPanicMessage(kind, ctx, loc, message);
    llvm::Value* msgVal = ctx.createStringLiteral(fullMsg);
    ctx.builder.CreateCall(panicFn, {msgVal});
    ctx.builder.CreateUnreachable();
}

void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const std::string& message,
    const SourceLocation& loc
) {
    emitPanicImpl(kind, ctx, message, loc);
}

void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    BaseAST* node
) {
    SourceLocation loc = node ? node->loc : SourceLocation();
    emitPanicImpl(kind, ctx, "", loc);
}

void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const std::string& message,
    BaseAST* node
) {
    SourceLocation loc = node ? node->loc : SourceLocation();
    emitPanicImpl(kind, ctx, message, loc);
}

void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const std::string& message
) {
    emitPanicImpl(kind, ctx, message, SourceLocation());
}

// ─── emitZeroCheck ─────────────────────────────────────────────────────────

llvm::Value* emitZeroCheck(
    llvm::Value* val,
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    const SourceLocation& loc
) {
    if (!val) return nullptr;

    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);

    // Cast to i64 if needed
    llvm::Value* checkedVal = val;
    if (checkedVal->getType() != i64) {
        checkedVal = ctx.builder.CreateIntCast(checkedVal, i64, false, "zero_check_cast");
    }

    llvm::Value* isZero = ctx.builder.CreateICmpEQ(
        checkedVal,
        llvm::ConstantInt::get(i64, 0),
        "is_zero"
    );

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "zero_check_continue",
        func
    );

    if (fallbackBlock) {
        // ─── Fallback mode: branch to fallback on zero ──────────────────────
        ctx.builder.CreateCondBr(isZero, fallbackBlock, continueBlock);
        ctx.builder.SetInsertPoint(continueBlock);
        return checkedVal;
    } else {
        // ─── Panic mode: call __lucid_panic on zero ─────────────────────────
        llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx,
            "zero_check_panic",
            func
        );
        ctx.builder.CreateCondBr(isZero, panicBlock, continueBlock);

        // ─── Panic block ─────────────────────────────────────────────────────
        ctx.builder.SetInsertPoint(panicBlock);
        emitPanic(kind, ctx, getRuntimeErrorMessage(kind), loc);
        ctx.builder.CreateUnreachable();

        ctx.builder.SetInsertPoint(continueBlock);
        return checkedVal;
    }
}

llvm::Value* emitZeroCheck(
    llvm::Value* val,
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    BaseAST* node
) {
    SourceLocation loc = node ? node->loc : SourceLocation();
    return emitZeroCheck(val, kind, ctx, fallbackBlock, loc);
}

// ─── emitBoundsCheck ──────────────────────────────────────────────────────

llvm::Value* emitBoundsCheck(
    llvm::Value* index,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    const SourceLocation& loc
) {
    if (!index || !length) return nullptr;

    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);

    // Cast to i64 if needed
    if (index->getType() != i64) {
        index = ctx.builder.CreateIntCast(index, i64, true, "bounds_index_cast");
    }
    if (length->getType() != i64) {
        length = ctx.builder.CreateIntCast(length, i64, false, "bounds_length_cast");
    }

    // ─── Check: 0 <= index < length ──────────────────────────────────────
    llvm::Value* isNegative = ctx.builder.CreateICmpSLT(index, llvm::ConstantInt::get(i64, 0), "idx_negative");
    llvm::Value* isGE = ctx.builder.CreateICmpSGE(index, length, "idx_ge_len");
    llvm::Value* isOutOfBounds = ctx.builder.CreateOr(isNegative, isGE, "out_of_bounds");

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "bounds_check_continue",
        func
    );

    if (fallbackBlock) {
        // ─── Fallback mode: branch to fallback on out-of-bounds ─────────────
        ctx.builder.CreateCondBr(isOutOfBounds, fallbackBlock, continueBlock);
        ctx.builder.SetInsertPoint(continueBlock);
        return index;
    } else {
        // ─── Panic mode: call __lucid_panic on out-of-bounds ────────────────
        llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx,
            "bounds_check_panic",
            func
        );
        ctx.builder.CreateCondBr(isOutOfBounds, panicBlock, continueBlock);

        // ─── Panic block ─────────────────────────────────────────────────────
        ctx.builder.SetInsertPoint(panicBlock);
        emitPanic(RuntimeErrorKind::ArrayIndexOutOfBounds, ctx, "", loc);
        ctx.builder.CreateUnreachable();

        ctx.builder.SetInsertPoint(continueBlock);
        return index;
    }
}

llvm::Value* emitBoundsCheck(
    llvm::Value* index,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    BaseAST* node
) {
    SourceLocation loc = node ? node->loc : SourceLocation();
    return emitBoundsCheck(index, length, ctx, fallbackBlock, loc);
}

// ─── emitSliceBoundsCheck ─────────────────────────────────────────────────

std::pair<llvm::Value*, llvm::Value*> emitSliceBoundsCheck(
    llvm::Value* start,
    llvm::Value* end,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    const SourceLocation& loc
) {
    if (!start || !end || !length) {
        return {nullptr, nullptr};
    }

    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);

    // Cast to i64 if needed
    if (start->getType() != i64) {
        start = ctx.builder.CreateIntCast(start, i64, true, "slice_start_cast");
    }
    if (end->getType() != i64) {
        end = ctx.builder.CreateIntCast(end, i64, true, "slice_end_cast");
    }
    if (length->getType() != i64) {
        length = ctx.builder.CreateIntCast(length, i64, false, "slice_length_cast");
    }

    // ─── Check: 0 <= start <= end <= length ──────────────────────────────
    llvm::Value* startNeg = ctx.builder.CreateICmpSLT(start, llvm::ConstantInt::get(i64, 0), "start_neg");
    llvm::Value* endNeg = ctx.builder.CreateICmpSLT(end, llvm::ConstantInt::get(i64, 0), "end_neg");
    llvm::Value* startGTEnd = ctx.builder.CreateICmpSGT(start, end, "start_gt_end");
    llvm::Value* endGTLen = ctx.builder.CreateICmpSGT(end, length, "end_gt_len");
    llvm::Value* isOutOfBounds = ctx.builder.CreateOr(startNeg, endNeg, "bounds_check_1");
    isOutOfBounds = ctx.builder.CreateOr(isOutOfBounds, startGTEnd, "bounds_check_2");
    isOutOfBounds = ctx.builder.CreateOr(isOutOfBounds, endGTLen, "bounds_check_3");

    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "slice_bounds_continue",
        func
    );

    if (fallbackBlock) {
        // ─── Fallback mode: branch to fallback on out-of-bounds ─────────────
        ctx.builder.CreateCondBr(isOutOfBounds, fallbackBlock, continueBlock);
        ctx.builder.SetInsertPoint(continueBlock);
        return {start, end};
    } else {
        // ─── Panic mode: call __lucid_panic on out-of-bounds ────────────────
        llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx,
            "slice_bounds_panic",
            func
        );
        ctx.builder.CreateCondBr(isOutOfBounds, panicBlock, continueBlock);

        // ─── Panic block ─────────────────────────────────────────────────────
        ctx.builder.SetInsertPoint(panicBlock);
        emitPanic(RuntimeErrorKind::SliceBoundsOutOfRange, ctx, "", loc);
        ctx.builder.CreateUnreachable();

        ctx.builder.SetInsertPoint(continueBlock);
        return {start, end};
    }
}

std::pair<llvm::Value*, llvm::Value*> emitSliceBoundsCheck(
    llvm::Value* start,
    llvm::Value* end,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    BaseAST* node
) {
    SourceLocation loc = node ? node->loc : SourceLocation();
    return emitSliceBoundsCheck(start, end, length, ctx, fallbackBlock, loc);
}

} // namespace codegen