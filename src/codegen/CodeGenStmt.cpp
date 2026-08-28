/// @file CodeGenStmt.cpp
/// @brief Implementation of statement lowering to LLVM IR.
///
/// ─── Design Principle ──────────────────────────────────────────────────────
/// CodeGen TRUSTS the AST. All validation is done by Sema. If Sema succeeded,
/// the AST is guaranteed to be well-formed. CodeGen should NOT validate or
/// report errors - it should only generate IR.
///
/// Assertions are used ONLY in debug builds to catch bugs in Sema.
/// Never use diagnostics in CodeGen for semantic errors.

#include "CodeGen.hpp"
#include "CodeGenType.hpp"
#include "support/CodeGenAlloca.hpp"
#include "support/CodeGenHelpers.hpp"
#include "support/CodeGenPanic.hpp"
#include "support/LLVMHelpers.hpp"
#include "support/Truthiness.hpp"
#include "intrinsic/LucidIntrinsicEmitter.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Verifier.h>

#include <cassert>

namespace codegen {

// =============================================================================
// Statement Lowering - Dispatch
// =============================================================================

void lowerStatement(StmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            lowerBlockStmt(stmt->as<BlockStmtAST>(), ctx);
            break;
        case ASTKind::IfStmt:
            lowerIfStmt(stmt->as<IfStmtAST>(), ctx);
            break;
        case ASTKind::SwitchStmt:
            lowerSwitchStmt(stmt->as<SwitchStmtAST>(), ctx);
            break;
        case ASTKind::ForStmt:
            lowerForStmt(stmt->as<ForStmtAST>(), ctx);
            break;
        case ASTKind::WhileStmt:
            lowerWhileStmt(stmt->as<WhileStmtAST>(), ctx);
            break;
        case ASTKind::DoWhileStmt:
            lowerDoWhileStmt(stmt->as<DoWhileStmtAST>(), ctx);
            break;
        case ASTKind::ReturnStmt:
            lowerReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
            break;
        case ASTKind::BreakStmt:
            lowerBreakStmt(stmt->as<BreakStmtAST>(), ctx);
            break;
        case ASTKind::ContinueStmt:
            lowerContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
            break;
        case ASTKind::ExprStmt:
            lowerExprStmt(stmt->as<ExprStmtAST>(), ctx);
            break;
        case ASTKind::DeclStmt:
            lowerDeclStmt(stmt->as<DeclStmtAST>(), ctx);
            break;
        case ASTKind::FuncRefStmt:
            lowerFuncRefStmt(stmt->as<FuncRefStmtAST>(), ctx);
            break;
        case ASTKind::AsyncStmt:
            lowerAsyncStmt(stmt->as<AsyncStmtAST>(), ctx);
            break;
        case ASTKind::AwaitStmt:
            lowerAwaitStmt(stmt->as<AwaitStmtAST>(), ctx);
            break;
        case ASTKind::SpawnStmt:
            lowerSpawnStmt(stmt->as<SpawnStmtAST>(), ctx);
            break;
        case ASTKind::JoinStmt:
            lowerJoinStmt(stmt->as<JoinStmtAST>(), ctx);
            break;
        default:
            // This should never happen - Sema would have rejected unknown kinds
            llvm_unreachable("Unsupported statement kind in CodeGen");
    }
}

// =============================================================================
// Block Statement
// =============================================================================

// emitScopeExitCallback moved to intrinsic/LucidIntrinsicEmitter.cpp -
// it's the codegen half of the #scope_exit intrinsic, so it belongs
// alongside emitLucidControlIntrinsic rather than here.

void lowerBlockStmt(BlockStmtAST* block, CodeGenContext& ctx) {
    // ─── 1. Push live scope ──────────────────────────────────────────────
    ctx.pushLiveScope();  // Creates a new LiveVariableTracker
    
    // ─── 2. Lower all statements ──────────────────────────────────────────
    for (StmtAST* stmt : block->stmts) {
        lowerStatement(stmt, ctx);
    }
    
    // ─── 3. Pop live scope ────────────────────────────────────────────────
    ctx.popLiveScope();
    // └── This calls emitScopeExitCleanup() automatically:
    //     - Releases closure environments
    //     - Frees dynamic arrays
    //     - Frees strings
    //     - Pops the LiveVariableTracker
    
    // ─── 4. Emit user #scope_exit callbacks ──────────────────────────────
    for (size_t i = block->scopeExits.size(); i > 0; --i) {
        const ScopeExitRegistration* reg = block->scopeExits[i - 1];
        emitScopeExitCallback(reg, ctx);
    }
}

// =============================================================================
// If Statement
// =============================================================================

void lowerIfStmt(IfStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    llvm::Value* cond = lowerExpression(stmt->condition, ctx);
    if (!cond) return;

    if (stmt->condition->isLValue) {
        llvm::Type* conditionType = getType(ctx, stmt->condition->resolvedType);
        if (conditionType) {
            cond = loadIfNeeded(cond, conditionType, ctx);
        }
    }
    cond = emitTruthiness(cond, stmt->condition->resolvedType, ctx);

    llvm::Function* func = ctx.getCurrentFunction();
    assert(func && "No current function");

    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_then", func);
    llvm::BasicBlock* elseBlock = nullptr;
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_merge", func);

    if (stmt->elseBranch) {
        elseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_else", func);
    } else {
        elseBlock = mergeBlock;
    }

    ctx.builder.CreateCondBr(cond, thenBlock, elseBlock);

    ctx.builder.SetInsertPoint(thenBlock);
    lowerStatement(stmt->thenBranch, ctx);
    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(mergeBlock);
    }

    if (stmt->elseBranch) {
        ctx.builder.SetInsertPoint(elseBlock);
        if (stmt->elseBranch->isa<IfStmtAST>()) {
            lowerIfStmt(stmt->elseBranch->as<IfStmtAST>(), ctx);
        } else {
            lowerStatement(stmt->elseBranch, ctx);
        }
        if (!ctx.builder.GetInsertBlock()->getTerminator()) {
            ctx.builder.CreateBr(mergeBlock);
        }
    }

    ctx.builder.SetInsertPoint(mergeBlock);
}

// =============================================================================
// Switch Statement
// =============================================================================

void lowerSwitchStmt(SwitchStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    llvm::Value* subject = lowerExpression(stmt->subject, ctx);
    if (!subject) return;

    if (stmt->subject->isLValue) {
        llvm::Type* elemType = getType(ctx, stmt->subject->resolvedType);
        if (elemType) {
            subject = loadIfNeeded(subject, elemType, ctx);
        }
        if (!subject) return;
    }

    llvm::Function* func = ctx.getCurrentFunction();
    assert(func && "No current function");

    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "switch_merge", func);

    // ─── Build case blocks ──────────────────────────────────────────────
    std::vector<llvm::BasicBlock*> caseBlocks;
    std::vector<llvm::ConstantInt*> caseValues;
    std::vector<llvm::BasicBlock*> caseBodyBlocks;

    for (const SwitchCaseAST* caseStmt : stmt->cases) {
        llvm::BasicBlock* caseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "case", func);
        caseBlocks.push_back(caseBlock);

        for (ExprAST* value : caseStmt->values) {
            if (value->isa<LiteralExprAST>()) {
                LiteralExprAST* lit = value->as<LiteralExprAST>();
                llvm::Value* val = lowerLiteralExpr(lit, ctx);
                if (val) {
                    if (llvm::ConstantInt* constInt = llvm::dyn_cast<llvm::ConstantInt>(val)) {
                        caseValues.push_back(constInt);
                        caseBodyBlocks.push_back(caseBlock);
                    }
                }
            }
            // Range cases are handled differently - Sema ensures they're valid
            else if (value->isa<RangeExprAST>()) {
                // Range cases are handled by if-else chains in CodeGen
                // For now, we skip them
            }
        }
    }

    // ─── Create default block ────────────────────────────────────────────
    llvm::BasicBlock* defaultBlock;
    if (stmt->defaultBody) {
        defaultBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "switch_default", func);
    } else {
        defaultBlock = mergeBlock;
    }

    // ─── Create switch instruction ──────────────────────────────────────
    if (!caseValues.empty()) {
        llvm::SwitchInst* switchInst = ctx.builder.CreateSwitch(
            subject,
            defaultBlock,
            caseValues.size()
        );

        for (size_t i = 0; i < caseValues.size(); ++i) {
            switchInst->addCase(caseValues[i], caseBodyBlocks[i]);
        }
    } else {
        ctx.builder.CreateBr(defaultBlock);
    }

    // ─── Lower case bodies ──────────────────────────────────────────────
    for (size_t i = 0; i < stmt->cases.size(); ++i) {
        const SwitchCaseAST* caseStmt = stmt->cases[i];
        llvm::BasicBlock* caseBlock = caseBlocks[i];

        ctx.builder.SetInsertPoint(caseBlock);
        lowerStatement(caseStmt->body, ctx);
        if (!ctx.builder.GetInsertBlock()->getTerminator()) {
            ctx.builder.CreateBr(mergeBlock);
        }
    }

    // ─── Lower default body ─────────────────────────────────────────────
    if (stmt->defaultBody) {
        ctx.builder.SetInsertPoint(defaultBlock);
        lowerStatement(stmt->defaultBody, ctx);
        if (!ctx.builder.GetInsertBlock()->getTerminator()) {
            ctx.builder.CreateBr(mergeBlock);
        }
    }

    ctx.builder.SetInsertPoint(mergeBlock);
}

// =============================================================================
// For Statement
// =============================================================================

// ─── Helper: Lower a range-based for loop ────────────────────────────────

static void lowerRangeForLoop(
    ForStmtAST* stmt,
    llvm::BasicBlock* headerBlock,
    llvm::BasicBlock* bodyBlock,
    llvm::BasicBlock* continueBlock,
    llvm::BasicBlock* exitBlock,
    CodeGenContext& ctx
) {
    // Sema guarantees this is a RangeExprAST
    RangeExprAST* range = stmt->iterable->as<RangeExprAST>();
    assert(range && "Range loop iterable must be RangeExprAST");

    // ─── Lower range bounds ──────────────────────────────────────────────
    llvm::Value* startVal = lowerExpression(range->lo, ctx);
    llvm::Value* endVal = lowerExpression(range->hi, ctx);
    if (!startVal || !endVal) return;

    // Load if l-values
    if (range->lo->isLValue) {
        llvm::Type* elemType = getType(ctx, range->lo->resolvedType);
        assert(elemType && "Range bound has no type");
        startVal = loadIfNeeded(startVal, elemType, ctx);
    }
    if (range->hi->isLValue) {
        llvm::Type* elemType = getType(ctx, range->hi->resolvedType);
        assert(elemType && "Range bound has no type");
        endVal = loadIfNeeded(endVal, elemType, ctx);
    }

    // ─── Get index type ──────────────────────────────────────────────────
    llvm::Type* idxType = getType(ctx, stmt->indexVar->type);
    assert(idxType && "Index variable has no type");

    if (startVal->getType() != idxType) {
        startVal = ctx.builder.CreateIntCast(startVal, idxType, true, "start_cast");
    }
    if (endVal->getType() != idxType) {
        endVal = ctx.builder.CreateIntCast(endVal, idxType, true, "end_cast");
    }

    // ─── Lower step value ────────────────────────────────────────────────
    llvm::Value* stepVal = llvm::ConstantInt::get(idxType, 1);
    if (stmt->step) {
        stepVal = lowerExpression(stmt->step, ctx);
        if (stepVal && stmt->step->isLValue) {
            llvm::Type* elemType = getType(ctx, stmt->step->resolvedType);
            if (elemType) {
                stepVal = loadIfNeeded(stepVal, elemType, ctx);
            }
        }
        if (stepVal && stepVal->getType() != idxType) {
            stepVal = ctx.builder.CreateIntCast(stepVal, idxType, true, "step_cast");
        }
    }

    // ─── Allocate and initialize loop variable ──────────────────────────
    llvm::AllocaInst* alloca = createAlloca(
        ctx.pool.lookup(stmt->indexVar->name),
        idxType,
        ctx
    );

    ctx.builder.CreateStore(startVal, alloca);
    ctx.storeValue(stmt->indexVar, alloca);
    stmt->indexVar->llvmAlloca = alloca;

    ctx.builder.CreateBr(headerBlock);

    // ─── Header block ────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(headerBlock);

    llvm::Value* current = ctx.builder.CreateLoad(idxType, alloca, "loop_current");

    llvm::Value* stepIsPositive = ctx.builder.CreateICmpSGT(
        stepVal,
        llvm::ConstantInt::get(idxType, 0),
        "step_positive"
    );

    llvm::Value* cond;
    if (range->isExclusive) {
        llvm::Value* condPositive = ctx.builder.CreateICmpSLT(current, endVal, "current_lt_end");
        llvm::Value* condNegative = ctx.builder.CreateICmpSGT(current, endVal, "current_gt_end");
        cond = ctx.builder.CreateSelect(stepIsPositive, condPositive, condNegative, "cond_exclusive");
    } else {
        llvm::Value* condPositive = ctx.builder.CreateICmpSLE(current, endVal, "current_le_end");
        llvm::Value* condNegative = ctx.builder.CreateICmpSGE(current, endVal, "current_ge_end");
        cond = ctx.builder.CreateSelect(stepIsPositive, condPositive, condNegative, "cond_inclusive");
    }

    ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);

    // ─── Body block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(bodyBlock);

    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.emitScopeExitCleanup();
        ctx.builder.CreateBr(continueBlock);
    }

    // ─── Continue block ──────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(continueBlock);

    llvm::Value* currentForInc = ctx.builder.CreateLoad(idxType, alloca, "loop_current_inc");
    llvm::Value* incremented = ctx.builder.CreateAdd(currentForInc, stepVal, "loop_increment");
    ctx.builder.CreateStore(incremented, alloca);

    ctx.builder.CreateBr(headerBlock);

    ctx.builder.SetInsertPoint(exitBlock);
}

// ─── Helper: Lower a collection-based for loop ───────────────────────────

static void lowerCollectionForLoop(
    ForStmtAST* stmt,
    llvm::BasicBlock* headerBlock,
    llvm::BasicBlock* bodyBlock,
    llvm::BasicBlock* continueBlock,
    llvm::BasicBlock* exitBlock,
    CodeGenContext& ctx
) {
    // Sema guarantees this is an array type
    TypeAST* iterableType = stmt->iterable->resolvedType;
    assert(iterableType && iterableType->isa<ArrayTypeAST>() &&
           "Collection loop iterable must be ArrayTypeAST");

    ArrayTypeAST* arrayType = iterableType->as<ArrayTypeAST>();
    llvm::Type* elemType = getType(ctx, arrayType->element);
    assert(elemType && "Array element has no type");

    // ─── Lower the collection expression ──────────────────────────────
    llvm::Value* collection = lowerExpression(stmt->iterable, ctx);
    if (!collection) return;

    if (stmt->iterable->isLValue) {
        llvm::Type* collectionType = getType(ctx, stmt->iterable->resolvedType);
        if (collectionType) {
            collection = loadIfNeeded(collection, collectionType, ctx);
        }
    }

    // ─── Get array length ──────────────────────────────────────────────
    llvm::Value* len = getArrayLength(collection, arrayType, ctx);
    assert(len && "Could not get array length");

    // ─── Get pointer to array data ──────────────────────────────────────
    llvm::Value* dataPtr = collection;
    if (arrayType->isFixed()) {
        dataPtr = ctx.builder.CreateConstGEP2_32(elemType, collection, 0, 0);
    }

    // ─── Allocate index variable ──────────────────────────────────────
    llvm::Type* int64Ty = llvm::Type::getInt64Ty(ctx.llvmCtx);
    llvm::AllocaInst* indexAlloca = createAlloca(
        stmt->indexVar ? ctx.pool.lookup(stmt->indexVar->name) : "_loop_idx",
        int64Ty,
        ctx
    );

    ctx.builder.CreateStore(llvm::ConstantInt::get(int64Ty, 0), indexAlloca);
    if (stmt->indexVar) {
        ctx.storeValue(stmt->indexVar, indexAlloca);
        stmt->indexVar->llvmAlloca = indexAlloca;
    }

    // ─── Allocate value variable ──────────────────────────────────────
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

    ctx.builder.CreateBr(headerBlock);

    // ─── Header block ────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(headerBlock);

    llvm::Value* currentIdx = ctx.builder.CreateLoad(int64Ty, indexAlloca, "loop_idx");

    llvm::Value* cond = ctx.builder.CreateICmpSLT(currentIdx, len, "idx_lt_len");
    ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);

    // ─── Body block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(bodyBlock);

    llvm::Value* elemPtr = ctx.builder.CreateGEP(elemType, dataPtr, currentIdx, "elem_ptr");
    llvm::Value* elemVal = ctx.builder.CreateLoad(elemType, elemPtr, "elem_val");

    if (valueAlloca) {
        ctx.builder.CreateStore(elemVal, valueAlloca);
    }

    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(continueBlock);
    }

    // ─── Continue block ──────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(continueBlock);

    llvm::Value* nextIdx = ctx.builder.CreateAdd(
        currentIdx,
        llvm::ConstantInt::get(int64Ty, 1),
        "idx_next"
    );
    ctx.builder.CreateStore(nextIdx, indexAlloca);

    ctx.builder.CreateBr(headerBlock);

    ctx.builder.SetInsertPoint(exitBlock);
}

void lowerForStmt(ForStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // Sema guarantees these are set
    assert(stmt->iterable && "ForStmt must have an iterable");
    assert(stmt->indexVar && "ForStmt must have an index variable");
    assert(stmt->body && "ForStmt must have a body");

    bool isRangeLoop = (stmt->valueVar == nullptr);

    llvm::Function* func = ctx.getCurrentFunction();
    assert(func && "No current function");

    llvm::BasicBlock* headerBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "for_header", func);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "for_body", func);
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "for_continue", func);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "for_exit", func);

    ctx.pushLoop(headerBlock, exitBlock, continueBlock);

    if (isRangeLoop) {
        lowerRangeForLoop(stmt, headerBlock, bodyBlock, continueBlock, exitBlock, ctx);
    } else {
        lowerCollectionForLoop(stmt, headerBlock, bodyBlock, continueBlock, exitBlock, ctx);
    }

    ctx.popLoop();
}

// =============================================================================
// While Statement
// =============================================================================

void lowerWhileStmt(WhileStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    llvm::Function* func = ctx.getCurrentFunction();
    assert(func && "No current function");

    llvm::BasicBlock* headerBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "while_header", func);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "while_body", func);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "while_exit", func);

    ctx.pushLoop(headerBlock, exitBlock, bodyBlock);

    ctx.builder.CreateBr(headerBlock);

    ctx.builder.SetInsertPoint(headerBlock);

    llvm::Value* cond = lowerExpression(stmt->condition, ctx);
    if (!cond) {
        ctx.popLoop();
        return;
    }

    if (stmt->condition->isLValue) {
        llvm::Type* conditionType = getType(ctx, stmt->condition->resolvedType);
        if (conditionType) {
            cond = loadIfNeeded(cond, conditionType, ctx);
        }
    }
    cond = emitTruthiness(cond, stmt->condition->resolvedType, ctx);

    ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);

    ctx.builder.SetInsertPoint(bodyBlock);

    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.emitScopeExitCleanup();
        ctx.builder.CreateBr(headerBlock);
    }

    ctx.builder.SetInsertPoint(exitBlock);

    ctx.popLoop();
}

// =============================================================================
// Do-While Statement
// =============================================================================

void lowerDoWhileStmt(DoWhileStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    llvm::Function* func = ctx.getCurrentFunction();
    assert(func && "No current function");

    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "dowhile_body", func);
    llvm::BasicBlock* headerBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "dowhile_header", func);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "dowhile_exit", func);

    ctx.pushLoop(headerBlock, exitBlock, headerBlock);

    ctx.builder.CreateBr(bodyBlock);

    ctx.builder.SetInsertPoint(bodyBlock);

    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(headerBlock);
    }

    ctx.builder.SetInsertPoint(headerBlock);

    llvm::Value* cond = lowerExpression(stmt->condition, ctx);
    if (!cond) {
        ctx.popLoop();
        return;
    }

    if (stmt->condition->isLValue) {
        llvm::Type* conditionType = getType(ctx, stmt->condition->resolvedType);
        if (conditionType) {
            cond = loadIfNeeded(cond, conditionType, ctx);
        }
    }
    cond = emitTruthiness(cond, stmt->condition->resolvedType, ctx);

    ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);

    ctx.builder.SetInsertPoint(exitBlock);

    ctx.popLoop();
}

// =============================================================================
// Return Statement
// =============================================================================

void lowerReturnStmt(ReturnStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    llvm::Function* func = ctx.getCurrentFunction();
    assert(func && "Return statement outside of function");

    llvm::Type* returnType = func->getReturnType();

    // ─── Check if this is the main function ──────────────────────────────
    bool isMain = false;
    std::string funcName = func->getName().str();
    if (funcName == "main" || funcName == "__lucid_main") {
        isMain = true;
    }

    // ─── Emit cleanup for ALL scopes before returning ──────────────────
    while (!ctx.liveTrackers.empty()) {
        ctx.emitScopeExitCleanup();
        ctx.liveTrackers.pop_back();
    }

    // ─── If this is main, call __lucid_shutdown() before returning ──────
    if (isMain) {
        llvm::Function* shutdownFn = ctx.getRuntimeFn(RuntimeFn::Shutdown);
        ctx.builder.CreateCall(shutdownFn, {});
    }

    if (stmt->value) {
        llvm::Value* returnVal = lowerExpression(stmt->value, ctx);
        if (!returnVal) return;

        if (stmt->value->isLValue) {
            llvm::Type* elemType = getType(ctx, stmt->value->resolvedType);
            assert(elemType && "Return value has no type");
            returnVal = loadIfNeeded(returnVal, elemType, ctx);
        }

        // Cast if needed
        if (returnVal->getType() != returnType) {
            if (returnVal->getType()->isIntegerTy() && returnType->isIntegerTy()) {
                if (getIntegerBitWidth(returnVal->getType()) < getIntegerBitWidth(returnType)) {
                    returnVal = ctx.builder.CreateSExt(returnVal, returnType);
                } else {
                    returnVal = ctx.builder.CreateTrunc(returnVal, returnType);
                }
            } else if (returnVal->getType()->isFloatingPointTy() && returnType->isFloatingPointTy()) {
                if (returnVal->getType()->getPrimitiveSizeInBits() < returnType->getPrimitiveSizeInBits()) {
                    returnVal = ctx.builder.CreateFPExt(returnVal, returnType);
                } else {
                    returnVal = ctx.builder.CreateFPTrunc(returnVal, returnType);
                }
            } else if (returnVal->getType()->isPointerTy() && returnType->isPointerTy()) {
                returnVal = ctx.builder.CreatePointerCast(returnVal, returnType);
            }
        }

        ctx.builder.CreateRet(returnVal);
    } else {
        assert(returnType->isVoidTy() && "Void return in non-void function");
        ctx.builder.CreateRetVoid();
    }
}

// =============================================================================
// Break Statement
// =============================================================================

void lowerBreakStmt(BreakStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    CodeGenContext::LoopInfo* loop = ctx.currentLoop();
    assert(loop && "Break statement outside of loop");

    ctx.emitScopeExitCleanup();
    ctx.builder.CreateBr(loop->exit);
}

// =============================================================================
// Continue Statement
// =============================================================================

void lowerContinueStmt(ContinueStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    CodeGenContext::LoopInfo* loop = ctx.currentLoop();
    assert(loop && "Continue statement outside of loop");

    ctx.emitScopeExitCleanup();
    ctx.builder.CreateBr(loop->continueTarget);
}

// =============================================================================
// Expression Statement
// =============================================================================

void lowerExprStmt(ExprStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // Just lower the expression - value is discarded
    llvm::Value* value = lowerExpression(stmt->expr, ctx);
    (void)value; // Silence unused warning
}

// =============================================================================
// Declaration Statement
// =============================================================================

void lowerDeclStmt(DeclStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;
    lowerDeclaration(stmt->decl, ctx);
}

// =============================================================================
// Function Reference Statement
// =============================================================================

void lowerFuncRefStmt(FuncRefStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    llvm::Value* target = lowerExpression(stmt->target, ctx);
    if (!target) return;

    stmt->resolvedFunction = llvm::dyn_cast<llvm::Function>(target);
}

// =============================================================================
// Concurrency Statements (Async, Await, Spawn, Join)
// =============================================================================

// ─── Async Statement ──────────────────────────────────────────────────────

void lowerAsyncStmt(AsyncStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    assert(ctx.getCurrentFunction() && "Async statement outside of function");

    // ─── Step 1: Get the call expression ────────────────────────────────────
    if (!stmt->call || !stmt->call->isa<CallExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, stmt->loc,
                                "async statement requires a call expression");
        return;
    }

    CallExprAST* call = stmt->call->as<CallExprAST>();

    // ─── 2. Get the runtime function ────────────────────────────────────────
    llvm::Function* asyncFn = ctx.getRuntimeFn(RuntimeFn::Async);

    // ─── 3. Get the callable ──────────────────────────────────────────────────
    llvm::PointerType* i8Ptr = llvm::PointerType::get(ctx.llvmCtx, 0);

    llvm::Value* callable = lowerExpression(call->callee, ctx);
    if (!callable) return;

    if (call->callee->isLValue) {
        llvm::Type* callableType = getType(ctx, call->callee->resolvedType);
        if (callableType) {
            callable = loadIfNeeded(callable, callableType, ctx);
        }
    }

    // ─── 4. Build the argument list ────────────────────────────────────────
    llvm::Value* argsPtr = llvm::ConstantPointerNull::get(i8Ptr);

    if (!call->args.empty()) {
        llvm::ArrayType* argsArrayType = llvm::ArrayType::get(i8Ptr, call->args.size());
        llvm::AllocaInst* argsArray = createAlloca("async_args", argsArrayType, ctx);

        for (size_t i = 0; i < call->args.size(); ++i) {
            ExprAST* arg = call->args[i];
            llvm::Value* argVal = lowerExpression(arg, ctx);
            if (!argVal) return;

            if (arg->isLValue) {
                llvm::Type* elemType = getType(ctx, arg->resolvedType);
                if (elemType) {
                    argVal = loadIfNeeded(argVal, elemType, ctx);
                }
            }

            llvm::Value* idx = llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx.llvmCtx),
                i
            );
            llvm::Value* elemPtr = ctx.builder.CreateInBoundsGEP(
                i8Ptr,
                argsArray,
                idx,
                "async_arg_ptr"
            );

            // Store the argument in an alloca to get a stable pointer
            llvm::AllocaInst* argAlloca = createAlloca(
                "async_arg_" + std::to_string(i),
                argVal->getType(),
                ctx
            );
            ctx.builder.CreateStore(argVal, argAlloca);
            llvm::Value* castedArg = ctx.builder.CreatePointerCast(
                argAlloca,
                i8Ptr,
                "async_arg_cast"
            );
            ctx.builder.CreateStore(castedArg, elemPtr);
        }

        argsPtr = ctx.builder.CreatePointerCast(argsArray, i8Ptr, "args_cast");
    }

    // ─── 5. Allocate the future handle storage ──────────────────────────────
    llvm::AllocaInst* futureHandle = createAlloca(
        ctx.pool.lookup(stmt->binding->name) + "_future",
        i8Ptr,
        ctx
    );

    // ─── 6. Call __lucid_async ──────────────────────────────────────────────
    llvm::Value* callablePtr = ctx.builder.CreatePointerCast(
        callable,
        i8Ptr,
        "callable_cast"
    );

    llvm::Value* result = ctx.builder.CreateCall(
        asyncFn,
        {callablePtr, argsPtr, futureHandle},
        "async_result"
    );

    // ─── 7. Store the future handle in the binding ──────────────────────────
    if (stmt->binding) {
        llvm::Value* bindingValue = ctx.lookupValue(stmt->binding);
        if (bindingValue) {
            ctx.builder.CreateStore(result, bindingValue);
        } else {
            llvm::Type* bindingType = getType(ctx, stmt->binding->type);
            if (bindingType) {
                bindingValue = createAlloca(
                    ctx.pool.lookup(stmt->binding->name),
                    bindingType,
                    ctx
                );
                ctx.builder.CreateStore(result, bindingValue);
                ctx.storeValue(stmt->binding, bindingValue);
                ctx.markAlive(stmt->binding);
            }
        }
    }

    if (stmt->binding) {
        ctx.markAlive(stmt->binding);
    }

    Trace::detail("Lowered async statement: ", ctx.pool.lookup(stmt->binding->name));
}

// ─── Await Statement ──────────────────────────────────────────────────────

void lowerAwaitStmt(AwaitStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── 1. Get the runtime function ────────────────────────────────────────
    // void __lucid_await(void* future_handle)
    llvm::Function* awaitFn = ctx.getRuntimeFn(RuntimeFn::Await);

    // ─── 2. Await each target ──────────────────────────────────────────────
    for (ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            // Sema should have caught this
            ctx.diagnostics.errorAt(DiagCode::Sem_AwaitNonAsync, target->loc,
                                    "await target must be an identifier");
            continue;
        }

        IdentifierExprAST* id = target->as<IdentifierExprAST>();
        ValueDeclAST* decl = id->resolvedDecl;
        if (!decl) {
            ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, target->loc,
                                    "identifier '", ctx.pool.lookup(id->name), 
                                    "' was not resolved by Sema");
            continue;
        }

        // ─── 3. Look up the binding value ──────────────────────────────────
        llvm::Value* bindingValue = ctx.lookupValue(decl);
        if (!bindingValue) {
            ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, target->loc,
                                    "no LLVM value for '", ctx.pool.lookup(id->name), "'");
            continue;
        }

        // ─── 4. Load the future handle from the binding ────────────────────
        llvm::Type* handleType = llvm::PointerType::get(ctx.llvmCtx, 0);
        llvm::Value* futureHandle = ctx.builder.CreateLoad(
            handleType,
            bindingValue,
            "future_handle_load"
        );

        // ─── 5. Call __lucid_await ──────────────────────────────────────────
        ctx.builder.CreateCall(awaitFn, {futureHandle});

        // ─── 6. Mark the binding as consumed ────────────────────────────────
        ctx.markConsumed(decl);
    }
}

// ─── Spawn Statement ──────────────────────────────────────────────────────

void lowerSpawnStmt(SpawnStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    assert(ctx.getCurrentFunction() && "Spawn statement outside of function");

    // ─── Step 1: Get the call expression ────────────────────────────────────
    if (!stmt->call || !stmt->call->isa<CallExprAST>()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, stmt->loc,
                                "spawn statement requires a call expression");
        return;
    }

    CallExprAST* call = stmt->call->as<CallExprAST>();

    // ─── 2. Get the runtime function ────────────────────────────────────────
    llvm::Function* spawnFn = ctx.getRuntimeFn(RuntimeFn::Spawn);

    llvm::PointerType* i8Ptr = llvm::PointerType::get(ctx.llvmCtx, 0);

    // ─── 3. Get the callable ──────────────────────────────────────────────────
    llvm::Value* callable = lowerExpression(call->callee, ctx);
    if (!callable) return;

    if (call->callee->isLValue) {
        llvm::Type* callableType = getType(ctx, call->callee->resolvedType);
        if (callableType) {
            callable = loadIfNeeded(callable, callableType, ctx);
        }
    }

    // ─── 4. Build the argument list ────────────────────────────────────────
    llvm::Value* argsPtr = llvm::ConstantPointerNull::get(i8Ptr);

    if (!call->args.empty()) {
        llvm::ArrayType* argsArrayType = llvm::ArrayType::get(i8Ptr, call->args.size());
        llvm::AllocaInst* argsArray = createAlloca("spawn_args", argsArrayType, ctx);

        for (size_t i = 0; i < call->args.size(); ++i) {
            ExprAST* arg = call->args[i];
            llvm::Value* argVal = lowerExpression(arg, ctx);
            if (!argVal) return;

            if (arg->isLValue) {
                llvm::Type* elemType = getType(ctx, arg->resolvedType);
                if (elemType) {
                    argVal = loadIfNeeded(argVal, elemType, ctx);
                }
            }

            llvm::Value* idx = llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx.llvmCtx),
                i
            );
            llvm::Value* elemPtr = ctx.builder.CreateInBoundsGEP(
                i8Ptr,
                argsArray,
                idx,
                "spawn_arg_ptr"
            );

            llvm::AllocaInst* argAlloca = createAlloca(
                "spawn_arg_" + std::to_string(i),
                argVal->getType(),
                ctx
            );
            ctx.builder.CreateStore(argVal, argAlloca);
            llvm::Value* castedArg = ctx.builder.CreatePointerCast(
                argAlloca,
                i8Ptr,
                "spawn_arg_cast"
            );
            ctx.builder.CreateStore(castedArg, elemPtr);
        }

        argsPtr = ctx.builder.CreatePointerCast(argsArray, i8Ptr, "args_cast");
    }

    // ─── 5. Handle discard pattern (_) ──────────────────────────────────────
    if (!stmt->binding) {
        llvm::Value* callablePtr = ctx.builder.CreatePointerCast(
            callable,
            i8Ptr,
            "callable_cast"
        );
        llvm::Value* nullHandle = llvm::ConstantPointerNull::get(i8Ptr);

        ctx.builder.CreateCall(
            spawnFn,
            {callablePtr, argsPtr, nullHandle},
            "spawn_discard"
        );

        Trace::detail("Lowered spawn discard statement");
        return;
    }

    // ─── 6. Named binding: fire and join later ─────────────────────────────
    llvm::AllocaInst* threadHandle = createAlloca(
        ctx.pool.lookup(stmt->binding->name) + "_thread",
        i8Ptr,
        ctx
    );

    llvm::Value* callablePtr = ctx.builder.CreatePointerCast(
        callable,
        i8Ptr,
        "callable_cast"
    );

    llvm::Value* result = ctx.builder.CreateCall(
        spawnFn,
        {callablePtr, argsPtr, threadHandle},
        "spawn_result"
    );

    // ─── 7. Store the thread handle in the binding ──────────────────────────
    if (stmt->binding) {
        llvm::Value* bindingValue = ctx.lookupValue(stmt->binding);
        if (bindingValue) {
            ctx.builder.CreateStore(result, bindingValue);
        } else {
            llvm::Type* bindingType = getType(ctx, stmt->binding->type);
            if (bindingType) {
                bindingValue = createAlloca(
                    ctx.pool.lookup(stmt->binding->name),
                    bindingType,
                    ctx
                );
                ctx.builder.CreateStore(result, bindingValue);
                ctx.storeValue(stmt->binding, bindingValue);
                ctx.markAlive(stmt->binding);
            }
        }
    }

    if (stmt->binding) {
        ctx.markAlive(stmt->binding);
    }

    Trace::detail("Lowered spawn statement: ", ctx.pool.lookup(stmt->binding->name));
}

// ─── Join Statement ──────────────────────────────────────────────────────

void lowerJoinStmt(JoinStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── 1. Get the runtime function ────────────────────────────────────────
    // void __lucid_join(void* thread_handle)
    llvm::Function* joinFn = ctx.getRuntimeFn(RuntimeFn::Join);

    // ─── 2. Join each target ──────────────────────────────────────────────
    for (ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_JoinNonSpawn, target->loc,
                                    "join target must be an identifier");
            continue;
        }

        IdentifierExprAST* id = target->as<IdentifierExprAST>();
        ValueDeclAST* decl = id->resolvedDecl;
        if (!decl) {
            ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, target->loc,
                                    "identifier '", ctx.pool.lookup(id->name), 
                                    "' was not resolved by Sema");
            continue;
        }

        // ─── 3. Look up the binding value ──────────────────────────────────
        llvm::Value* bindingValue = ctx.lookupValue(decl);
        if (!bindingValue) {
            ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, target->loc,
                                    "no LLVM value for '", ctx.pool.lookup(id->name), "'");
            continue;
        }

        // ─── 4. Load the thread handle from the binding ────────────────────
        llvm::Type* handleType = llvm::PointerType::get(ctx.llvmCtx, 0);
        llvm::Value* threadHandle = ctx.builder.CreateLoad(
            handleType,
            bindingValue,
            "thread_handle_load"
        );

        // ─── 5. Call __lucid_join ───────────────────────────────────────────
        ctx.builder.CreateCall(joinFn, {threadHandle});

        // ─── 6. Mark the binding as consumed ────────────────────────────────
        ctx.markConsumed(decl);
    }
}

} // namespace codegen