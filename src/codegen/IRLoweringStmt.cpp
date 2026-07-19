/**
 * @file IRLoweringStmt.cpp
 * @brief Lowers Lucid statements to LLVM IR.
 * 
 * @responsibility Handles all statement nodes: blocks, if/else, loops,
 *                 switches, returns, and assignments.
 * 
 * @related_files
 *   - src/codegen/IRLowering.hpp - Declarations
 *   - src/codegen/IRLoweringDecl.cpp - Declaration lowering
 *   - src/codegen/IRLoweringExpr.cpp - Expression lowering
 *   - src/codegen/IRLoweringBuilder.hpp - Helper builders
 */

#include "IRLowering.hpp"
#include "IRLoweringBuilder.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - Main Dispatcher
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerStmt(IRLowering& lowerer, StmtAST* stmt) {
    if (!stmt) {
        return;
    }

    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            lowerBlockStmt(lowerer, stmt->as<BlockStmtAST>());
            break;
        case ASTKind::IfStmt:
            lowerIfStmt(lowerer, stmt->as<IfStmtAST>());
            break;
        case ASTKind::SwitchStmt:
            lowerSwitchStmt(lowerer, stmt->as<SwitchStmtAST>());
            break;
        case ASTKind::ForStmt:
            lowerForStmt(lowerer, stmt->as<ForStmtAST>());
            break;
        case ASTKind::WhileStmt:
            lowerWhileStmt(lowerer, stmt->as<WhileStmtAST>());
            break;
        case ASTKind::ReturnStmt:
            lowerReturnStmt(lowerer, stmt->as<ReturnStmtAST>());
            break;
        case ASTKind::ExprStmt:
            lowerExprStmt(lowerer, stmt->as<ExprStmtAST>());
            break;
        case ASTKind::AssignExpr:
            lowerAssignStmt(lowerer, stmt->as<AssignExprAST>());
            break;
        default:
            // Ignore other statement types
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - Block Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerBlockStmt(IRLowering& lowerer, BlockStmtAST* block) {
    if (!block) {
        return;
    }

    // Enter a new scope for the block
    lowerer.enterScope();

    // Lower each statement in the block
    for (StmtPtr stmt : block->stmts) {
        lowerStmt(lowerer, stmt);

        // Check if the block was terminated (return/break/continue)
        auto* insertBlock = lowerer.getBuilder().GetInsertBlock();
        if (insertBlock && insertBlock->getTerminator()) {
            break;
        }
    }

    // Exit the block scope
    lowerer.exitScope();
}

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - If Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerIfStmt(IRLowering& lowerer, IfStmtAST* ifStmt) {
    if (!ifStmt) {
        return;
    }

    auto* function = lowerer.currentFunction().function;

    // Lower the condition
    auto* condition = IRExprLowering::lowerExpr(lowerer, ifStmt->condition);
    if (!condition) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower if condition");
    }

    // Convert to boolean if needed
    if (!condition->getType()->isIntegerTy(1)) {
        condition = lowerer.getBuilder().CreateICmpNE(
            condition,
            llvm::ConstantInt::get(condition->getType(), 0),
            "ifcond"
        );
    }

    // Create blocks
    auto* thenBlock = llvm::BasicBlock::Create(lowerer.getContext(), "then", function);
    auto* mergeBlock = llvm::BasicBlock::Create(lowerer.getContext(), "ifcont", function);

    // Check if there's an else branch
    bool hasElse = (ifStmt->elseBranch != nullptr);

    auto* elseBlock = hasElse 
        ? llvm::BasicBlock::Create(lowerer.getContext(), "else", function)
        : mergeBlock;

    // Branch based on condition
    lowerer.getBuilder().CreateCondBr(condition, thenBlock, elseBlock);

    // Lower then branch
    lowerer.getBuilder().SetInsertPoint(thenBlock);
    lowerStmt(lowerer, ifStmt->thenBranch);
    if (!lowerer.getBuilder().GetInsertBlock()->getTerminator()) {
        lowerer.getBuilder().CreateBr(mergeBlock);
    }

    // Lower else branch (if present)
    if (hasElse) {
        lowerer.getBuilder().SetInsertPoint(elseBlock);
        
        // Check if else branch is an IfStmt (else if) or BlockStmt
        if (ifStmt->elseBranch->kind == ASTKind::IfStmt) {
            lowerStmt(lowerer, ifStmt->elseBranch);
        } else {
            lowerStmt(lowerer, ifStmt->elseBranch);
        }
        
        if (!lowerer.getBuilder().GetInsertBlock()->getTerminator()) {
            lowerer.getBuilder().CreateBr(mergeBlock);
        }
    }

    // Set insertion point to merge block
    lowerer.getBuilder().SetInsertPoint(mergeBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - Switch Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerSwitchStmt(IRLowering& lowerer, SwitchStmtAST* switchStmt) {
    if (!switchStmt) {
        return;
    }

    auto* function = lowerer.currentFunction().function;

    // Lower the subject
    auto* subject = IRExprLowering::lowerExpr(lowerer, switchStmt->subject);
    if (!subject) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower switch subject");
    }

    // Create blocks
    auto* defaultBlock = llvm::BasicBlock::Create(lowerer.getContext(), "switch.default", function);
    auto* mergeBlock = llvm::BasicBlock::Create(lowerer.getContext(), "switch.end", function);

    // Create switch instruction
    auto* switchInst = lowerer.getBuilder().CreateSwitch(subject, defaultBlock, 
                                                         switchStmt->cases.size());

    // Lower each case
    for (SwitchCasePtr caseNode : switchStmt->cases) {
        auto* caseBlock = llvm::BasicBlock::Create(lowerer.getContext(), "switch.case", function);

        // Add case values to switch
        for (ExprPtr value : caseNode->values) {
            auto* caseValue = IRExprLowering::lowerExpr(lowerer, value);
            if (caseValue) {
                // Convert to constant if possible
                if (auto* constValue = llvm::dyn_cast<llvm::ConstantInt>(caseValue)) {
                    switchInst->addCase(constValue, caseBlock);
                }
            }
        }

        // Lower case body
        lowerer.getBuilder().SetInsertPoint(caseBlock);
        if (caseNode->body) {
            lowerStmt(lowerer, caseNode->body);
        }
        if (!lowerer.getBuilder().GetInsertBlock()->getTerminator()) {
            lowerer.getBuilder().CreateBr(mergeBlock);
        }
    }

    // Lower default body
    lowerer.getBuilder().SetInsertPoint(defaultBlock);
    if (switchStmt->defaultBody) {
        lowerStmt(lowerer, switchStmt->defaultBody);
    }
    if (!lowerer.getBuilder().GetInsertBlock()->getTerminator()) {
        lowerer.getBuilder().CreateBr(mergeBlock);
    }

    lowerer.getBuilder().SetInsertPoint(mergeBlock);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - For Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerForStmt(IRLowering& lowerer, ForStmtAST* forStmt) {
    if (!forStmt) {
        return;
    }

    auto* function = lowerer.currentFunction().function;

    // For now, only support range loops
    // Collection loops would require iterators

    // Lower the iterable to get the range
    auto* iterable = IRExprLowering::lowerExpr(lowerer, forStmt->iterable);
    if (!iterable) {
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower for iterable");
    }

    // Create blocks
    auto* conditionBlock = llvm::BasicBlock::Create(lowerer.getContext(), "for.cond", function);
    auto* bodyBlock = llvm::BasicBlock::Create(lowerer.getContext(), "for.body", function);
    auto* incrementBlock = llvm::BasicBlock::Create(lowerer.getContext(), "for.inc", function);
    auto* exitBlock = llvm::BasicBlock::Create(lowerer.getContext(), "for.end", function);

    // Allocate index variable (if present)
    llvm::Value* indexVar = nullptr;
    llvm::Value* endValue = nullptr;
    
    if (forStmt->indexVar) {
        std::string name = lowerer.internedToString(forStmt->indexVar->name);
        indexVar = lowerer.allocateLocal(name, llvm::Type::getInt64Ty(lowerer.getContext()));
        
        // Initialize index to 0
        lowerer.getBuilder().CreateStore(
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), 0),
            indexVar
        );

        // Extract end value from iterable (assuming it's a range)
        // FIXME: Proper range extraction
        endValue = llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), 10);
    }

    // Save loop context for break/continue
    IRLowering::LoopContext loopCtx;
    loopCtx.conditionBlock = conditionBlock;
    loopCtx.bodyBlock = bodyBlock;
    loopCtx.incrementBlock = incrementBlock;
    loopCtx.exitBlock = exitBlock;
    lowerer.pushLoop(loopCtx);

    // Jump to condition
    lowerer.getBuilder().CreateBr(conditionBlock);

    // Condition block
    lowerer.getBuilder().SetInsertPoint(conditionBlock);
    if (indexVar) {
        auto* index = lowerer.getBuilder().CreateLoad(
            llvm::Type::getInt64Ty(lowerer.getContext()),
            indexVar,
            "index"
        );
        auto* cond = lowerer.getBuilder().CreateICmpSLT(index, endValue, "for.cond");
        lowerer.getBuilder().CreateCondBr(cond, bodyBlock, exitBlock);
    } else {
        // Infinite loop if no index
        lowerer.getBuilder().CreateBr(bodyBlock);
    }

    // Body block
    lowerer.getBuilder().SetInsertPoint(bodyBlock);
    lowerer.enterScope();

    // Lower value variable (if present)
    if (forStmt->valueVar) {
        // Load current value from iterable
        // FIXME: Implement proper iteration
        auto* zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), 0);
        auto* value = lowerer.getBuilder().CreateLoad(
            llvm::Type::getInt64Ty(lowerer.getContext()),
            zero,
            "value"
        );
        std::string name = lowerer.internedToString(forStmt->valueVar->name);
        lowerer.storeLocal(name, value);
    }

    // Lower body
    lowerStmt(lowerer, forStmt->body);

    lowerer.exitScope();

    // Jump to increment if no terminator
    if (!lowerer.getBuilder().GetInsertBlock()->getTerminator()) {
        lowerer.getBuilder().CreateBr(incrementBlock);
    }

    // Increment block
    lowerer.getBuilder().SetInsertPoint(incrementBlock);
    if (indexVar) {
        auto* index = lowerer.getBuilder().CreateLoad(
            llvm::Type::getInt64Ty(lowerer.getContext()),
            indexVar,
            "index"
        );
        auto* inc = lowerer.getBuilder().CreateAdd(
            index,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(lowerer.getContext()), 1),
            "index.inc"
        );
        lowerer.getBuilder().CreateStore(inc, indexVar);
    }
    lowerer.getBuilder().CreateBr(conditionBlock);

    // Exit block
    lowerer.getBuilder().SetInsertPoint(exitBlock);

    // Pop loop context
    lowerer.popLoop();
}

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - While Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerWhileStmt(IRLowering& lowerer, WhileStmtAST* whileStmt) {
    if (!whileStmt) {
        return;
    }

    auto* function = lowerer.currentFunction().function;

    // Create blocks
    auto* conditionBlock = llvm::BasicBlock::Create(lowerer.getContext(), "while.cond", function);
    auto* bodyBlock = llvm::BasicBlock::Create(lowerer.getContext(), "while.body", function);
    auto* exitBlock = llvm::BasicBlock::Create(lowerer.getContext(), "while.end", function);

    // Save loop context for break/continue
    IRLowering::LoopContext loopCtx;
    loopCtx.conditionBlock = conditionBlock;
    loopCtx.bodyBlock = bodyBlock;
    loopCtx.exitBlock = exitBlock;
    lowerer.pushLoop(loopCtx);

    // Jump to condition
    lowerer.getBuilder().CreateBr(conditionBlock);

    // Condition block
    lowerer.getBuilder().SetInsertPoint(conditionBlock);
    auto* condition = IRExprLowering::lowerExpr(lowerer, whileStmt->condition);
    if (!condition) {
        lowerer.popLoop();
        throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                              "Failed to lower while condition");
    }
    
    // Convert to boolean if needed
    if (!condition->getType()->isIntegerTy(1)) {
        condition = lowerer.getBuilder().CreateICmpNE(
            condition,
            llvm::ConstantInt::get(condition->getType(), 0),
            "while.cond"
        );
    }
    
    lowerer.getBuilder().CreateCondBr(condition, bodyBlock, exitBlock);

    // Body block
    lowerer.getBuilder().SetInsertPoint(bodyBlock);
    lowerer.enterScope();
    lowerStmt(lowerer, whileStmt->body);
    lowerer.exitScope();

    if (!lowerer.getBuilder().GetInsertBlock()->getTerminator()) {
        lowerer.getBuilder().CreateBr(conditionBlock);
    }

    // Exit block
    lowerer.getBuilder().SetInsertPoint(exitBlock);

    lowerer.popLoop();
}

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - Return Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerReturnStmt(IRLowering& lowerer, ReturnStmtAST* returnStmt) {
    if (!returnStmt) {
        return;
    }

    if (returnStmt->values.empty()) {
        // Void return
        lowerer.getBuilder().CreateRetVoid();
        lowerer.currentFunction().hasReturn = true;
    } else if (returnStmt->values.size() == 1) {
        // Single return value
        auto* value = IRExprLowering::lowerExpr(lowerer, returnStmt->values[0]);
        if (value) {
            lowerer.getBuilder().CreateRet(value);
            lowerer.currentFunction().hasReturn = true;
        }
    } else {
        // Multiple return values - pack into a struct
        std::vector<llvm::Value*> values;
        for (ExprPtr expr : returnStmt->values) {
            auto* value = IRExprLowering::lowerExpr(lowerer, expr);
            if (value) {
                values.push_back(value);
            }
        }

        // Build struct type from return values
        std::vector<llvm::Type*> types;
        for (auto* v : values) {
            types.push_back(v->getType());
        }
        auto* structType = llvm::StructType::get(lowerer.getContext(), types);

        // Pack values into struct
        llvm::Value* packed = llvm::UndefValue::get(structType);
        for (size_t i = 0; i < values.size(); ++i) {
            packed = lowerer.getBuilder().CreateInsertValue(packed, values[i], i);
        }

        lowerer.getBuilder().CreateRet(packed);
        lowerer.currentFunction().hasReturn = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - Expression Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerExprStmt(IRLowering& lowerer, ExprStmtAST* exprStmt) {
    if (!exprStmt) {
        return;
    }

    // Lower the expression and discard the result
    // If the expression has side effects, they will be emitted during lowering
    IRExprLowering::lowerExpr(lowerer, exprStmt->expr);
}

// ─────────────────────────────────────────────────────────────────────────────
// IRStmtLowering - Assignment Statements
// ─────────────────────────────────────────────────────────────────────────────

void IRStmtLowering::lowerAssignStmt(IRLowering& lowerer, AssignExprAST* assign) {
    if (!assign) {
        return;
    }

    // Lower the left-hand side (lvalue) and right-hand side (rvalue)
    auto* lhs = IRExprLowering::lowerExpr(lowerer, assign->lhs);
    auto* rhs = IRExprLowering::lowerExpr(lowerer, assign->rhs);

    if (lhs && rhs) {
        // Store rhs into lhs
        // If lhs is a pointer, store directly; otherwise, it's an lvalue
        if (lhs->getType()->isPointerTy()) {
            // Store the value into the address
            auto* store = lowerer.getBuilder().CreateStore(rhs, lhs);
            
            // Assignment expressions return the assigned value
            // For statement form, we don't need the result
        } else {
            // If lhs is not a pointer, it should be an lvalue
            // FIXME: Handle lvalue properly
            throw IRLoweringError(IRLoweringError::Kind::UnsupportedNode,
                                  "Assignment target is not a valid lvalue");
        }
    }
}