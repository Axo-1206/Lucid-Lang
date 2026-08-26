#include "CodeGenHelpers.hpp"
#include "CodeGenAlloca.hpp"
#include "LLVMHelpers.hpp"
#include "codegen/CodeGen.hpp"
#include "core/ast/ExprAST.hpp"

namespace codegen {

llvm::Value* getArrayLength(llvm::Value* target, ArrayTypeAST* arrayType, CodeGenContext& ctx) {
    if (!target || !arrayType) return nullptr;

    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.llvmCtx);

    switch (arrayType->arrayKind) {
        case ArrayKind::Fixed: {
            // Fixed array: length is a compile-time constant
            return llvm::ConstantInt::get(i64Ty, arrayType->size);
        }

        case ArrayKind::Dynamic: {
            // Dynamic array: the length is stored at the beginning.
            // With opaque pointers, we cannot use getPointerElementType().
            // We need to know the layout from the AST/type system.
            //
            // The target is a pointer to the array data.
            // The length is stored before the data: [length: i64][data: T*]
            // We need to offset back by 8 bytes to get the length.
            
            // First, check if the target is a pointer (it should be)
            if (isPointerType(target->getType())) {
                // For dynamic arrays, we assume the runtime stores the length
                // immediately before the data. So we subtract 8 bytes from the
                // data pointer to get the length pointer.
                llvm::Value* lenPtr = ctx.builder.CreatePtrToInt(
                    target,
                    i64Ty,
                    "data_ptr_int"
                );
                lenPtr = ctx.builder.CreateSub(
                    lenPtr,
                    llvm::ConstantInt::get(i64Ty, 8),
                    "len_ptr_int"
                );
                lenPtr = ctx.builder.CreateIntToPtr(
                    lenPtr,
                    llvm::PointerType::get(ctx.llvmCtx, 0),
                    "len_ptr"
                );
                return ctx.builder.CreateLoad(i64Ty, lenPtr, "array_len");
            }
            
            // Fallback: return a placeholder
            ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                                      "dynamic array length extraction not fully implemented");
            return llvm::ConstantInt::get(i64Ty, 0);
        }

        case ArrayKind::Slice: {
            // Slice: { ptr, len, cap }
            // The target is the slice struct value (not a pointer).
            // We need to extract the len field (index 1).
            if (isStructType(target->getType())) {
                llvm::StructType* structType = llvm::cast<llvm::StructType>(target->getType());
                if (structType->getNumElements() > 1) {
                    llvm::Value* len = ctx.builder.CreateExtractValue(
                        target,
                        1,
                        "slice_len"
                    );
                    return len;
                }
            }
            
            // If target is a pointer to a slice struct:
            if (isPointerType(target->getType())) {
                // With opaque pointers, we can't get the pointee type directly.
                // But we can use the fact that slices are { ptr, len, cap }.
                // We can GEP to the len field using an i8* base.
                llvm::Value* base = ctx.builder.CreatePointerCast(
                    target,
                    llvm::PointerType::get(ctx.llvmCtx, 0),
                    "slice_base"
                );
                llvm::Value* lenPtr = ctx.builder.CreateConstGEP1_32(
                    llvm::Type::getInt8Ty(ctx.llvmCtx),
                    base,
                    8,  // offset of len field after ptr (assuming ptr is 8 bytes)
                    "slice_len_ptr"
                );
                return ctx.builder.CreateLoad(i64Ty, lenPtr, "slice_len");
            }
            
            ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                                      "slice length extraction not fully implemented");
            return llvm::ConstantInt::get(i64Ty, 0);
        }

        default:
            return nullptr;
    }
}

static void lowerRangeForLoop(
    ForStmtAST* stmt,
    llvm::BasicBlock* headerBlock,
    llvm::BasicBlock* bodyBlock,
    llvm::BasicBlock* continueBlock,
    llvm::BasicBlock* exitBlock,
    CodeGenContext& ctx
) {
    llvm::Function* func = ctx.getCurrentFunction();

    // ─── 1. Lower range bounds ─────────────────────────────────────────────
    if (!stmt->iterable || !stmt->iterable->isa<RangeExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidIterator, stmt->iterable->loc,
                                "invalid range loop: expected range expression");
        return;
    }

    RangeExprAST* range = stmt->iterable->as<RangeExprAST>();

    // Lower start and end values
    llvm::Value* startVal = lowerExpression(range->lo, ctx);
    llvm::Value* endVal = lowerExpression(range->hi, ctx);

    if (!startVal || !endVal) {
        return;
    }

    // If bounds are l-values, load them with explicit element type
    if (range->lo->isLValue) {
        llvm::Type* elemType = getType(ctx, range->lo->resolvedType);
        if (elemType) {
            startVal = loadIfNeeded(startVal, elemType, ctx);
        }
    }
    if (range->hi->isLValue) {
        llvm::Type* elemType = getType(ctx, range->hi->resolvedType);
        if (elemType) {
            endVal = loadIfNeeded(endVal, elemType, ctx);
        }
    }

    if (!startVal || !endVal) {
        return;
    }

    // Cast both to the same type (use the index variable's type)
    llvm::Type* idxType = getType(ctx, stmt->indexVar->type);
    if (!idxType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, stmt->indexVar->loc,
                                "loop variable has invalid type");
        return;
    }

    if (startVal->getType() != idxType) {
        startVal = ctx.builder.CreateIntCast(startVal, idxType, true, "start_cast");
    }
    if (endVal->getType() != idxType) {
        endVal = ctx.builder.CreateIntCast(endVal, idxType, true, "end_cast");
    }

    // ─── 2. Lower step value ──────────────────────────────────────────────
    llvm::Value* stepVal = llvm::ConstantInt::get(idxType, 1);

    if (stmt->step) {
        stepVal = lowerExpression(stmt->step, ctx);
        if (!stepVal) {
            return;
        }
        if (stmt->step->isLValue) {
            llvm::Type* elemType = getType(ctx, stmt->step->resolvedType);
            if (elemType) {
                stepVal = loadIfNeeded(stepVal, elemType, ctx);
            }
        }
        if (!stepVal) {
            return;
        }
        if (stepVal->getType() != idxType) {
            stepVal = ctx.builder.CreateIntCast(stepVal, idxType, true, "step_cast");
        }
    }

    // ─── 3. Allocate and initialize loop variable ─────────────────────────
    llvm::AllocaInst* alloca = createAlloca(
        ctx.pool.lookup(stmt->indexVar->name),
        idxType,
        ctx
    );

    // Store initial value
    ctx.builder.CreateStore(startVal, alloca);
    ctx.storeValue(stmt->indexVar, alloca);
    stmt->indexVar->llvmAlloca = alloca;

    // ─── 4. Branch to header ──────────────────────────────────────────────
    ctx.builder.CreateBr(headerBlock);

    // ─── 5. Header block (condition check) ────────────────────────────────
    ctx.builder.SetInsertPoint(headerBlock);

    // Load current value
    llvm::Value* current = ctx.builder.CreateLoad(idxType, alloca, "loop_current");

    // Check if step is positive or negative
    llvm::Value* stepIsPositive = ctx.builder.CreateICmpSGT(
        stepVal,
        llvm::ConstantInt::get(idxType, 0),
        "step_positive"
    );

    // Condition: if step > 0: current <= end, else: current >= end
    llvm::Value* cond;
    if (range->isExclusive) {
        // Exclusive end: current < end for positive step, current > end for negative
        llvm::Value* condPositive = ctx.builder.CreateICmpSLT(current, endVal, "current_lt_end");
        llvm::Value* condNegative = ctx.builder.CreateICmpSGT(current, endVal, "current_gt_end");
        cond = ctx.builder.CreateSelect(stepIsPositive, condPositive, condNegative, "cond_exclusive");
    } else {
        // Inclusive end: current <= end for positive step, current >= end for negative
        llvm::Value* condPositive = ctx.builder.CreateICmpSLE(current, endVal, "current_le_end");
        llvm::Value* condNegative = ctx.builder.CreateICmpSGE(current, endVal, "current_ge_end");
        cond = ctx.builder.CreateSelect(stepIsPositive, condPositive, condNegative, "cond_inclusive");
    }

    ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);

    // ─── 6. Body block ─────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(bodyBlock);

    // Lower the loop body
    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    // Branch to continue block if no terminator
    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(continueBlock);
    }

    // ─── 7. Continue block (increment) ────────────────────────────────────
    ctx.builder.SetInsertPoint(continueBlock);

    // Load current value
    llvm::Value* currentForInc = ctx.builder.CreateLoad(idxType, alloca, "loop_current_inc");

    // Add step
    llvm::Value* incremented = ctx.builder.CreateAdd(currentForInc, stepVal, "loop_increment");
    ctx.builder.CreateStore(incremented, alloca);

    // Branch back to header
    ctx.builder.CreateBr(headerBlock);

    // ─── 8. Exit block ─────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(exitBlock);
}

static void lowerCollectionForLoop(
    ForStmtAST* stmt,
    llvm::BasicBlock* headerBlock,
    llvm::BasicBlock* bodyBlock,
    llvm::BasicBlock* continueBlock,
    llvm::BasicBlock* exitBlock,
    CodeGenContext& ctx
) {
    llvm::Function* func = ctx.getCurrentFunction();

    // ─── 1. Lower the collection expression ────────────────────────────────
    llvm::Value* collection = lowerExpression(stmt->iterable, ctx);
    if (!collection) {
        return;
    }

    if (stmt->iterable->isLValue) {
        llvm::Type* elemType = getType(ctx, stmt->iterable->resolvedType);
        if (elemType) {
            collection = loadIfNeeded(collection, elemType, ctx);
        }
        if (!collection) {
            return;
        }
    }

    // ─── 2. Get array type and length ─────────────────────────────────────
    TypeAST* iterableType = stmt->iterable->resolvedType;
    if (!iterableType || !iterableType->isa<ArrayTypeAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidIterator, stmt->iterable->loc,
                                "collection loop requires an array type");
        return;
    }

    ArrayTypeAST* arrayType = iterableType->as<ArrayTypeAST>();
    llvm::Type* elemType = getType(ctx, arrayType->element);
    if (!elemType) {
        return;
    }

    // ─── 3. Get array length ──────────────────────────────────────────────
    llvm::Value* len = getArrayLength(collection, arrayType, ctx);
    if (!len) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidArrayElement, stmt->iterable->loc,
                                "could not determine array length");
        return;
    }

    // ─── 4. Get pointer to array data ─────────────────────────────────────
    llvm::Value* dataPtr = collection;
    if (arrayType->isFixed()) {
        dataPtr = ctx.builder.CreateConstGEP2_32(elemType, collection, 0, 0);
    }

    // ─── 5. Allocate index variable ───────────────────────────────────────
    llvm::Type* int64Ty = llvm::Type::getInt64Ty(ctx.llvmCtx);

    // If index is not discarded (_), allocate it
    llvm::AllocaInst* indexAlloca = nullptr;
    if (stmt->indexVar) {
        indexAlloca = createAlloca(
            ctx.pool.lookup(stmt->indexVar->name),
            int64Ty,
            ctx
        );
        ctx.builder.CreateStore(
            llvm::ConstantInt::get(int64Ty, 0),
            indexAlloca
        );
        ctx.storeValue(stmt->indexVar, indexAlloca);
        stmt->indexVar->llvmAlloca = indexAlloca;
    }

    // ─── 6. Allocate value variable ──────────────────────────────────────
    llvm::AllocaInst* valueAlloca = nullptr;
    if (stmt->valueVar) {
        valueAlloca = createAlloca(
            ctx.pool.lookup(stmt->valueVar->name),
            elemType,
            ctx
        );
        ctx.storeValue(stmt->valueVar, valueAlloca);
        stmt->valueVar->llvmAlloca = valueAlloca;
    }

    // ─── 7. Branch to header ──────────────────────────────────────────────
    ctx.builder.CreateBr(headerBlock);

    // ─── 8. Header block (condition check) ────────────────────────────────
    ctx.builder.SetInsertPoint(headerBlock);

    // We need a PHI node for the index if we don't have an alloca
    // For simplicity, we'll use an alloca even for discarded index
    llvm::AllocaInst* tempAlloca = createAlloca("_loop_idx", int64Ty, ctx);
    if (!stmt->indexVar) {
        ctx.builder.CreateStore(
            llvm::ConstantInt::get(int64Ty, 0),
            tempAlloca
        );
    }

    // Load current index (from alloca or temp)
    llvm::Value* currentIdx = ctx.builder.CreateLoad(
        int64Ty,
        stmt->indexVar ? indexAlloca : tempAlloca,
        "loop_idx"
    );

    // Check: idx < len
    llvm::Value* cond = ctx.builder.CreateICmpSLT(currentIdx, len, "idx_lt_len");
    ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);

    // ─── 9. Body block ─────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(bodyBlock);

    // Load the current element
    llvm::Value* elemPtr = ctx.builder.CreateGEP(elemType, dataPtr, currentIdx, "elem_ptr");
    llvm::Value* elemVal = ctx.builder.CreateLoad(elemType, elemPtr, "elem_val");

    // Store element in value variable
    if (valueAlloca) {
        ctx.builder.CreateStore(elemVal, valueAlloca);
    }

    // Lower the loop body
    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    // Branch to continue block if no terminator
    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(continueBlock);
    }

    // ─── 10. Continue block (increment) ──────────────────────────────────
    ctx.builder.SetInsertPoint(continueBlock);

    // Increment index
    llvm::Value* nextIdx = ctx.builder.CreateAdd(
        currentIdx,
        llvm::ConstantInt::get(int64Ty, 1),
        "idx_next"
    );

    // Store in the appropriate alloca
    if (stmt->indexVar && indexAlloca) {
        ctx.builder.CreateStore(nextIdx, indexAlloca);
    } else {
        ctx.builder.CreateStore(nextIdx, tempAlloca);
    }

    // Branch back to header
    ctx.builder.CreateBr(headerBlock);

    // ─── 11. Exit block ────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(exitBlock);
}

}