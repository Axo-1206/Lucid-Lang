/// @file CodeGenStmt.cpp
/// @brief Implementation of statement lowering to LLVM IR.

#include "CodeGen.hpp"
#include "CodeGenType.hpp"
#include "support/CodeGenAlloca.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Verifier.h>

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
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidUnary, stmt->loc,
                                    "unsupported statement kind: ",
                                    debug::kindToString(stmt->kind));
            break;
    }
}

// =============================================================================
// Block Statement
// =============================================================================

void emitScopeExitCallback(const ScopeExitRegistration* reg, CodeGenContext& ctx) {
    if (!reg) return;

    // ─── Get the callback function ──────────────────────────────────────
    llvm::Value* callback = nullptr;
    if (reg->callback) {
        callback = ctx.lookupFunction(reg->callback);
        if (!callback) {
            callback = reg->callback->llvmFunction;
        }
    } else {
        // It's a closure - we need to evaluate the callable expression
        // For now, just warn and return
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, reg->callExpr->loc,
                                  "scope_exit with closure callback not fully implemented");
        return;
    }

    if (!callback) {
        ctx.diagnostics.errorAt(DiagCode::Sem_NotCallable, reg->callExpr->loc,
                                "scope_exit callback not found");
        return;
    }

    // ─── Lower arguments ──────────────────────────────────────────────────
    std::vector<llvm::Value*> args;
    for (ExprAST* arg : reg->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return;
        }
        // If argument is an l-value, load it
        if (arg->isLValue) {
            argVal = loadIfNeeded(argVal, true, ctx);
        }
        args.push_back(argVal);
    }

    // ─── Create the call ──────────────────────────────────────────────────
    ctx.builder.CreateCall(
        llvm::dyn_cast<llvm::Function>(callback),
        args
    );
}

void lowerBlockStmt(BlockStmtAST* block, CodeGenContext& ctx) {
    if (!block) return;

    // ─── Lower each statement in the block ──────────────────────────────
    for (StmtAST* stmt : block->stmts) {
        lowerStatement(stmt, ctx);
    }

    // ─── Emit scope-exit callbacks (LIFO order) ─────────────────────────
    // Scope exits execute in reverse registration order
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

    // ─── Lower condition ──────────────────────────────────────────────────
    llvm::Value* cond = lowerExpression(stmt->condition, ctx);
    if (!cond) {
        return;
    }

    // ─── Get the condition as a bool ──────────────────────────────────────
    if (!cond->getType()->isIntegerTy(1)) {
        cond = ctx.builder.CreateICmpNE(cond,
            llvm::Constant::getNullValue(cond->getType()));
    }

    // ─── Create blocks ────────────────────────────────────────────────────
    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_then", func);
    llvm::BasicBlock* elseBlock = nullptr;
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_merge", func);

    if (stmt->elseBranch) {
        elseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "if_else", func);
    } else {
        elseBlock = mergeBlock;
    }

    ctx.builder.CreateCondBr(cond, thenBlock, elseBlock);

    // ─── Then branch ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(thenBlock);
    lowerStatement(stmt->thenBranch, ctx);
    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(mergeBlock);
    }

    // ─── Else branch ──────────────────────────────────────────────────────
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

    // ─── Merge block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(mergeBlock);

    // ─── Handle unreachable code ──────────────────────────────────────────
    // If the then branch always transfers control (return/break/continue),
    // the merge block might be unreachable. That's fine.
}

// =============================================================================
// Switch Statement
// =============================================================================

void lowerSwitchStmt(SwitchStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Lower subject ─────────────────────────────────────────────────────
    llvm::Value* subject = lowerExpression(stmt->subject, ctx);
    if (!subject) {
        return;
    }

    // ─── If subject is an l-value, load it ──────────────────────────────
    if (stmt->subject->isLValue) {
        subject = loadIfNeeded(subject, true, ctx);
        if (!subject) return;
    }

    // ─── Get the subject type ────────────────────────────────────────────
    const TypeAST* subjectType = stmt->subject->resolvedType;
    if (!subjectType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidSwitchType, stmt->subject->loc,
                                "switch subject has no type");
        return;
    }

    // ─── Create blocks ────────────────────────────────────────────────────
    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* switchBlock = ctx.builder.GetInsertBlock();
    llvm::BasicBlock* defaultBlock = nullptr;
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "switch_merge", func);

    // ─── Build case blocks ────────────────────────────────────────────────
    std::vector<llvm::BasicBlock*> caseBlocks;
    std::vector<llvm::ConstantInt*> caseValues;
    std::vector<llvm::BasicBlock*> caseBodyBlocks;

    // Create a block for each case
    for (const SwitchCasePtr caseStmt : stmt->cases) {
        llvm::BasicBlock* caseBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "case", func);
        caseBlocks.push_back(caseBlock);

        // Get the case value(s)
        for (ExprAST* value : caseStmt->values) {
            // Lower the case value - should be a constant
            // For now, assume it's a literal
            if (value->isa<LiteralExprAST>()) {
                LiteralExprAST* lit = value->as<LiteralExprAST>();
                // Try to get the integer value
                llvm::Value* val = lowerLiteralExpr(lit, ctx);
                if (val) {
                    if (llvm::ConstantInt* constInt = llvm::dyn_cast<llvm::ConstantInt>(val)) {
                        caseValues.push_back(constInt);
                        caseBodyBlocks.push_back(caseBlock);
                    }
                }
            } else if (value->isa<RangeExprAST>()) {
                // Range case - need to handle ranges separately
                // For now, just skip
                ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, value->loc,
                                          "range cases not fully implemented yet");
            } else {
                ctx.diagnostics.errorAt(DiagCode::Sem_InvalidSwitchType, value->loc,
                                        "case value must be a literal");
            }
        }
    }

    // ─── Create default block ─────────────────────────────────────────────
    if (stmt->defaultBody) {
        defaultBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "switch_default", func);
    } else {
        defaultBlock = mergeBlock;
    }

    // ─── Create switch instruction ──────────────────────────────────────
    // If we have case values, create a switch instruction
    if (!caseValues.empty()) {
        llvm::SwitchInst* switchInst = ctx.builder.CreateSwitch(
            subject,
            defaultBlock,
            caseValues.size()
        );

        // Add cases
        for (size_t i = 0; i < caseValues.size(); ++i) {
            switchInst->addCase(caseValues[i], caseBodyBlocks[i]);
        }
    } else {
        // No case values - just branch to default
        ctx.builder.CreateBr(defaultBlock);
    }

    // ─── Lower each case body ─────────────────────────────────────────────
    for (size_t i = 0; i < stmt->cases.size(); ++i) {
        const SwitchCasePtr caseStmt = stmt->cases[i];
        llvm::BasicBlock* caseBlock = caseBlocks[i];

        ctx.builder.SetInsertPoint(caseBlock);

        lowerStatement(caseStmt->body, ctx);

        // If the case body doesn't have a terminator, branch to merge
        if (!ctx.builder.GetInsertBlock()->getTerminator()) {
            ctx.builder.CreateBr(mergeBlock);
        }
    }

    // ─── Lower default body ──────────────────────────────────────────────
    if (stmt->defaultBody) {
        ctx.builder.SetInsertPoint(defaultBlock);
        lowerStatement(stmt->defaultBody, ctx);
        if (!ctx.builder.GetInsertBlock()->getTerminator()) {
            ctx.builder.CreateBr(mergeBlock);
        }
    }

    // ─── Merge block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(mergeBlock);
}

// =============================================================================
// For Statement
// =============================================================================

void lowerForStmt(ForStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Determine if this is a range loop or collection loop ────────────
    bool isRangeLoop = (stmt->valueVar == nullptr);

    // ─── Create loop blocks ──────────────────────────────────────────────
    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* headerBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "for_header", func);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "for_body", func);
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "for_continue", func);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "for_exit", func);

    // ─── Push loop context ─────────────────────────────────────────────────
    ctx.pushLoop(headerBlock, exitBlock, continueBlock);

    // ─── Lower initialization ─────────────────────────────────────────────
    if (isRangeLoop) {
        // Range loop: for i int in 0..10
        if (stmt->iterable && stmt->iterable->isa<RangeExprAST>()) {
            const RangeExprAST* range = stmt->iterable->as<RangeExprAST>();

            // Lower range bounds
            llvm::Value* startVal = lowerExpression(range->lo, ctx);
            llvm::Value* endVal = lowerExpression(range->hi, ctx);

            if (!startVal || !endVal) {
                ctx.popLoop();
                return;
            }

            // If bounds are l-values, load them
            if (range->lo->isLValue) {
                startVal = loadIfNeeded(startVal, true, ctx);
            }
            if (range->hi->isLValue) {
                endVal = loadIfNeeded(endVal, true, ctx);
            }

            // Store the loop variable
            if (stmt->indexVar) {
                // Create alloca for the loop variable
                llvm::Type* varType = getType(ctx, stmt->indexVar->type);
                llvm::AllocaInst* alloca = createAlloca(
                    ctx.pool.lookup(stmt->indexVar->name),
                    varType,
                    ctx
                );

                // Store the initial value
                ctx.builder.CreateStore(startVal, alloca);
                ctx.storeValue(stmt->indexVar, alloca);
            }

            // Branch to header
            ctx.builder.CreateBr(headerBlock);
        } else {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidIterator, stmt->iterable->loc,
                                    "invalid range loop");
            ctx.popLoop();
            return;
        }
    } else {
        // Collection loop: for i int, v int in nums
        // TODO: Implement collection iteration
        ctx.builder.CreateBr(headerBlock);
    }

    // ─── Header block (condition check) ──────────────────────────────────
    ctx.builder.SetInsertPoint(headerBlock);

    if (isRangeLoop) {
        if (stmt->indexVar) {
            llvm::Value* varValue = ctx.lookupValue(stmt->indexVar);
            if (!varValue) {
                ctx.popLoop();
                return;
            }

            // Load the current value - with opaque pointers, use the type from the AST
            llvm::Type* varType = getType(ctx, stmt->indexVar->type);
            llvm::Value* current = ctx.builder.CreateLoad(varType, varValue);

            // Get the end value
            const RangeExprAST* range = stmt->iterable->as<RangeExprAST>();
            llvm::Value* endVal = lowerExpression(range->hi, ctx);
            if (!endVal) {
                ctx.popLoop();
                return;
            }
            if (range->hi->isLValue) {
                endVal = loadIfNeeded(endVal, true, ctx);
            }

            // Compare current < end
            bool isInclusive = !range->isExclusive;
            llvm::Value* cond;
            if (isInclusive) {
                cond = ctx.builder.CreateICmpSLE(current, endVal);
            } else {
                cond = ctx.builder.CreateICmpSLT(current, endVal);
            }

            ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);
        } else {
            ctx.popLoop();
            return;
        }
    } else {
        // Collection loop condition
        // TODO: Implement collection iteration
        ctx.builder.CreateBr(bodyBlock);
    }

    // ─── Body block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(bodyBlock);

    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(continueBlock);
    }

    // ─── Continue block (increment) ──────────────────────────────────────
    ctx.builder.SetInsertPoint(continueBlock);

    if (isRangeLoop) {
        if (stmt->indexVar) {
            llvm::Value* varValue = ctx.lookupValue(stmt->indexVar);
            if (!varValue) {
                ctx.popLoop();
                return;
            }

            // Load current value
            llvm::Type* varType = getType(ctx, stmt->indexVar->type);
            llvm::Value* current = ctx.builder.CreateLoad(varType, varValue);

            // Add step (default 1)
            llvm::Value* step = llvm::ConstantInt::get(
                current->getType(),
                1
            );

            // If step is specified, use it
            if (stmt->step) {
                llvm::Value* stepVal = lowerExpression(stmt->step, ctx);
                if (stepVal) {
                    if (stmt->step->isLValue) {
                        stepVal = loadIfNeeded(stepVal, true, ctx);
                    }
                    step = stepVal;
                }
            }

            // Store the incremented value
            llvm::Value* incremented = ctx.builder.CreateAdd(current, step);
            ctx.builder.CreateStore(incremented, varValue);
        }
    }

    // Branch back to header
    ctx.builder.CreateBr(headerBlock);

    // ─── Exit block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(exitBlock);

    // Pop the loop context
    ctx.popLoop();
}

// =============================================================================
// While Statement
// =============================================================================

void lowerWhileStmt(WhileStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Create loop blocks ──────────────────────────────────────────────
    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* headerBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "while_header", func);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "while_body", func);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "while_exit", func);

    // ─── Push loop context ─────────────────────────────────────────────────
    ctx.pushLoop(headerBlock, exitBlock, bodyBlock);

    // ─── Branch to header ──────────────────────────────────────────────────
    ctx.builder.CreateBr(headerBlock);

    // ─── Header block (condition check) ──────────────────────────────────
    ctx.builder.SetInsertPoint(headerBlock);

    // Lower condition
    llvm::Value* cond = lowerExpression(stmt->condition, ctx);
    if (!cond) {
        ctx.popLoop();
        return;
    }

    // Get condition as bool
    if (!cond->getType()->isIntegerTy(1)) {
        cond = ctx.builder.CreateICmpNE(cond,
            llvm::Constant::getNullValue(cond->getType()));
    }

    ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);

    // ─── Body block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(bodyBlock);

    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(headerBlock);
    }

    // ─── Exit block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(exitBlock);

    // Pop the loop context
    ctx.popLoop();
}

// =============================================================================
// Do-While Statement
// =============================================================================

void lowerDoWhileStmt(DoWhileStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Create loop blocks ──────────────────────────────────────────────
    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "dowhile_body", func);
    llvm::BasicBlock* headerBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "dowhile_header", func);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "dowhile_exit", func);

    // ─── Push loop context ─────────────────────────────────────────────────
    ctx.pushLoop(headerBlock, exitBlock, headerBlock);

    // ─── Branch to body ────────────────────────────────────────────────────
    ctx.builder.CreateBr(bodyBlock);

    // ─── Body block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(bodyBlock);

    if (stmt->body) {
        lowerStatement(stmt->body, ctx);
    }

    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        ctx.builder.CreateBr(headerBlock);
    }

    // ─── Header block (condition check) ──────────────────────────────────
    ctx.builder.SetInsertPoint(headerBlock);

    // Lower condition
    llvm::Value* cond = lowerExpression(stmt->condition, ctx);
    if (!cond) {
        ctx.popLoop();
        return;
    }

    // Get condition as bool
    if (!cond->getType()->isIntegerTy(1)) {
        cond = ctx.builder.CreateICmpNE(cond,
            llvm::Constant::getNullValue(cond->getType()));
    }

    ctx.builder.CreateCondBr(cond, bodyBlock, exitBlock);

    // ─── Exit block ──────────────────────────────────────────────────────
    ctx.builder.SetInsertPoint(exitBlock);

    // Pop the loop context
    ctx.popLoop();
}

// =============================================================================
// Return Statement
// =============================================================================

void lowerReturnStmt(ReturnStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Get the current function ────────────────────────────────────────
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidBreak, stmt->loc,
                                "return statement outside of function");
        return;
    }

    // ─── Get the return type ──────────────────────────────────────────────
    llvm::Type* returnType = func->getReturnType();

    // ─── Handle return value ──────────────────────────────────────────────
    if (stmt->value) {
        // Non-void return
        llvm::Value* returnVal = lowerExpression(stmt->value, ctx);
        if (!returnVal) {
            return;
        }

        // If return value is an l-value, load it
        if (stmt->value->isLValue) {
            returnVal = loadIfNeeded(returnVal, true, ctx);
            if (!returnVal) return;
        }

        // Check if the return value type matches the function return type
        // If not, cast it
        if (returnVal->getType() != returnType) {
            if (returnVal->getType()->isIntegerTy() && returnType->isIntegerTy()) {
                // Integer conversion
                if (returnVal->getType()->getIntegerBitWidth() < returnType->getIntegerBitWidth()) {
                    returnVal = ctx.builder.CreateSExt(returnVal, returnType);
                } else {
                    returnVal = ctx.builder.CreateTrunc(returnVal, returnType);
                }
            } else if (returnVal->getType()->isFloatingPointTy() && returnType->isFloatingPointTy()) {
                // Float conversion
                if (returnVal->getType()->getPrimitiveSizeInBits() < returnType->getPrimitiveSizeInBits()) {
                    returnVal = ctx.builder.CreateFPExt(returnVal, returnType);
                } else {
                    returnVal = ctx.builder.CreateFPTrunc(returnVal, returnType);
                }
            } else if (returnVal->getType()->isPointerTy() && returnType->isPointerTy()) {
                // Pointer conversion
                returnVal = ctx.builder.CreatePointerCast(returnVal, returnType);
            } else {
                ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, stmt->value->loc,
                                        "return type mismatch: expected ",
                                        debug::typeToString(func->getReturnType(), ctx.pool),
                                        " got ",
                                        debug::typeToString(stmt->value->resolvedType, ctx.pool));
                return;
            }
        }

        ctx.builder.CreateRet(returnVal);
    } else {
        // Void return
        if (!returnType->isVoidTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, stmt->loc,
                                    "void return in non-void function");
            return;
        }
        ctx.builder.CreateRetVoid();
    }
}

// =============================================================================
// Break Statement
// =============================================================================

void lowerBreakStmt(BreakStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Get the current loop context ────────────────────────────────────
    CodeGenContext::LoopInfo* loop = ctx.currentLoop();
    if (!loop) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidBreak, stmt->loc,
                                "break statement outside of loop");
        return;
    }

    // ─── Branch to the loop exit block ────────────────────────────────────
    ctx.builder.CreateBr(loop->exit);
}

// =============================================================================
// Continue Statement
// =============================================================================

void lowerContinueStmt(ContinueStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Get the current loop context ────────────────────────────────────
    CodeGenContext::LoopInfo* loop = ctx.currentLoop();
    if (!loop) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidContinue, stmt->loc,
                                "continue statement outside of loop");
        return;
    }

    // ─── Branch to the continue target ────────────────────────────────────
    ctx.builder.CreateBr(loop->continueTarget);
}

// =============================================================================
// Expression Statement
// =============================================================================

void lowerExprStmt(ExprStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Lower the expression ─────────────────────────────────────────────
    llvm::Value* value = lowerExpression(stmt->expr, ctx);
    if (!value) {
        return;
    }

    // ─── If the expression is an l-value and has side effects, load it ────
    // For example, if the expression is a function call that returns a value
    // but we're discarding it, we still need to evaluate the function call.
    // The value is already computed by lowerExpression.
    // We don't need to do anything with the value.
}

// =============================================================================
// Declaration Statement
// =============================================================================

void lowerDeclStmt(DeclStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Lower the declaration ────────────────────────────────────────────
    lowerDeclaration(stmt->decl, ctx);
}

// =============================================================================
// Function Reference Statement
// =============================================================================

void lowerFuncRefStmt(FuncRefStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Lower the target expression ──────────────────────────────────────
    llvm::Value* target = lowerExpression(stmt->target, ctx);
    if (!target) {
        return;
    }

    // ─── Store the resolved function pointer ─────────────────────────────
    stmt->resolvedFunction = llvm::dyn_cast<llvm::Function>(target);
}

// =============================================================================
// Concurrency Statements (Async, Await, Spawn, Join)
// =============================================================================

// ─── Async Statement ──────────────────────────────────────────────────────

void lowerAsyncStmt(AsyncStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Get the runtime async function ───────────────────────────────────
    llvm::Function* asyncFunc = ctx.getRuntimeFunction("__lucid_async");
    if (!asyncFunc) {
        llvm::Type* voidPtrTy = llvm::PointerType::get(ctx.llvmCtx, 0);
        llvm::FunctionType* asyncType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {voidPtrTy, voidPtrTy, voidPtrTy},
            false
        );
        asyncFunc = llvm::Function::Create(
            asyncType,
            llvm::Function::ExternalLinkage,
            "__lucid_async",
            ctx.module
        );
        ctx.setRuntimeFunction("__lucid_async", asyncFunc);
    }

    // ─── Lower the call expression ──────────────────────────────────────
    llvm::Value* callResult = lowerExpression(stmt->call, ctx);
    if (!callResult) {
        return;
    }

    // ─── Store the result in the binding ──────────────────────────────────
    if (stmt->binding) {
        llvm::Value* bindingValue = ctx.lookupValue(stmt->binding);
        if (!bindingValue) {
            llvm::Type* bindingType = getType(ctx, stmt->binding->type);
            if (!bindingType) {
                return;
            }
            bindingValue = createAlloca(
                ctx.pool.lookup(stmt->binding->name),
                bindingType,
                ctx
            );
            ctx.storeValue(stmt->binding, bindingValue);
        }

        ctx.builder.CreateStore(callResult, bindingValue);
        stmt->binding->llvmAlloca = llvm::dyn_cast<llvm::AllocaInst>(bindingValue);
    }

    // ─── Call the runtime async function ─────────────────────────────────
    std::vector<llvm::Value*> args = {
        callResult,
        llvm::Constant::getNullValue(llvm::PointerType::get(ctx.llvmCtx, 0)),
        llvm::Constant::getNullValue(llvm::PointerType::get(ctx.llvmCtx, 0))
    };

    ctx.builder.CreateCall(asyncFunc, args);
}

// ─── Await Statement ──────────────────────────────────────────────────────

void lowerAwaitStmt(AwaitStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Get the runtime await function ───────────────────────────────────
    llvm::Function* awaitFunc = ctx.getRuntimeFunction("__lucid_await");
    if (!awaitFunc) {
        llvm::FunctionType* awaitType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {llvm::PointerType::get(ctx.llvmCtx, 0)},
            false
        );
        awaitFunc = llvm::Function::Create(
            awaitType,
            llvm::Function::ExternalLinkage,
            "__lucid_await",
            ctx.module
        );
        ctx.setRuntimeFunction("__lucid_await", awaitFunc);
    }

    // ─── Await each target ─────────────────────────────────────────────────
    for (ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_AwaitNonAsync, target->loc,
                                    "await target must be an identifier");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        
        // CodeGenContext doesn't have lookup by name.
        // This should have been resolved by Sema.
        // For now, we'll emit a warning and continue.
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, id->loc,
                                  "variable lookup by name not fully implemented in CodeGen");
        continue;
    }
}

// ─── Spawn Statement ──────────────────────────────────────────────────────

void lowerSpawnStmt(SpawnStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Get the runtime spawn function ───────────────────────────────────
    llvm::Function* spawnFunc = ctx.getRuntimeFunction("__lucid_spawn");
    if (!spawnFunc) {
        llvm::Type* voidPtrTy = llvm::PointerType::get(ctx.llvmCtx, 0);
        llvm::FunctionType* spawnType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {voidPtrTy, voidPtrTy},
            false
        );
        spawnFunc = llvm::Function::Create(
            spawnType,
            llvm::Function::ExternalLinkage,
            "__lucid_spawn",
            ctx.module
        );
        ctx.setRuntimeFunction("__lucid_spawn", spawnFunc);
    }

    // ─── Handle discard pattern ────────────────────────────────────────────
    if (!stmt->binding) {
        llvm::Value* callResult = lowerExpression(stmt->call, ctx);
        if (!callResult) {
            return;
        }

        std::vector<llvm::Value*> args = {
            callResult,
            llvm::Constant::getNullValue(llvm::PointerType::get(ctx.llvmCtx, 0))
        };

        ctx.builder.CreateCall(spawnFunc, args);
        return;
    }

    // ─── Lower the call expression ──────────────────────────────────────
    llvm::Value* callResult = lowerExpression(stmt->call, ctx);
    if (!callResult) {
        return;
    }

    // ─── Store the result in the binding ──────────────────────────────────
    if (stmt->binding) {
        llvm::Value* bindingValue = ctx.lookupValue(stmt->binding);
        if (!bindingValue) {
            llvm::Type* bindingType = getType(ctx, stmt->binding->type);
            if (!bindingType) {
                return;
            }
            bindingValue = createAlloca(
                ctx.pool.lookup(stmt->binding->name),
                bindingType,
                ctx
            );
            ctx.storeValue(stmt->binding, bindingValue);
        }

        ctx.builder.CreateStore(callResult, bindingValue);
        stmt->binding->llvmAlloca = llvm::dyn_cast<llvm::AllocaInst>(bindingValue);
    }

    // ─── Call the runtime spawn function ─────────────────────────────────
    std::vector<llvm::Value*> args = {
        callResult,
        llvm::Constant::getNullValue(llvm::PointerType::get(ctx.llvmCtx, 0))
    };

    ctx.builder.CreateCall(spawnFunc, args);
}

// ─── Join Statement ──────────────────────────────────────────────────────

void lowerJoinStmt(JoinStmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;

    // ─── Get the runtime join function ───────────────────────────────────
    llvm::Function* joinFunc = ctx.getRuntimeFunction("__lucid_join");
    if (!joinFunc) {
        llvm::FunctionType* joinType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {llvm::PointerType::get(ctx.llvmCtx, 0)},
            false
        );
        joinFunc = llvm::Function::Create(
            joinType,
            llvm::Function::ExternalLinkage,
            "__lucid_join",
            ctx.module
        );
        ctx.setRuntimeFunction("__lucid_join", joinFunc);
    }

    // ─── Join each target ──────────────────────────────────────────────────
    for (ExprAST* target : stmt->targets) {
        if (!target->isa<IdentifierExprAST>()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_JoinNonSpawn, target->loc,
                                    "join target must be an identifier");
            continue;
        }

        const IdentifierExprAST* id = target->as<IdentifierExprAST>();
        
        // CodeGenContext doesn't have lookup by name.
        // This should have been resolved by Sema.
        // For now, we'll emit a warning and continue.
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, id->loc,
                                  "variable lookup by name not fully implemented in CodeGen");
        continue;
    }
}

} // namespace codegen