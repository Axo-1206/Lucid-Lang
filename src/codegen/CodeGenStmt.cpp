// CodeGenStmt.cpp
#include "CodeGenContext.hpp"

namespace codegen {

void lowerStatement(const StmtAST* stmt, CodeGenContext& ctx) {
    if (!stmt) return;
    
    switch (stmt->kind) {
        case ASTKind::BlockStmt:
            lowerBlock(stmt->as<BlockStmtAST>(), ctx);
            break;
        case ASTKind::ExprStmt:
            lowerExprStmt(stmt->as<ExprStmtAST>(), ctx);
            break;
        case ASTKind::ReturnStmt:
            lowerReturnStmt(stmt->as<ReturnStmtAST>(), ctx);
            break;
        case ASTKind::IfStmt:
            lowerIfStmt(stmt->as<IfStmtAST>(), ctx);
            break;
        case ASTKind::ForStmt:
            lowerForStmt(stmt->as<ForStmtAST>(), ctx);
            break;
        case ASTKind::WhileStmt:
            lowerWhileStmt(stmt->as<WhileStmtAST>(), ctx);
            break;
        case ASTKind::BreakStmt:
            lowerBreakStmt(stmt->as<BreakStmtAST>(), ctx);
            break;
        case ASTKind::ContinueStmt:
            lowerContinueStmt(stmt->as<ContinueStmtAST>(), ctx);
            break;
        default:
            break;
    }
}

void lowerBlock(const BlockStmtAST* block, CodeGenContext& ctx) {
    ctx.pushScope();
    for (const StmtPtr stmt : block->stmts) {
        lowerStatement(stmt, ctx);
    }
    ctx.popScope();
}

void lowerReturnStmt(const ReturnStmtAST* stmt, CodeGenContext& ctx) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) return;
    
    if (stmt->value) {
        llvm::Value* val = lowerExpression(stmt->value, ctx);
        if (val) {
            ctx.builder.CreateRet(val);
        }
    } else {
        ctx.builder.CreateRetVoid();
    }
}

void lowerIfStmt(const IfStmtAST* stmt, CodeGenContext& ctx) {
    llvm::Value* cond = lowerExpression(stmt->condition, ctx);
    if (!cond) return;
    
    llvm::Function* func = ctx.getCurrentFunction();
    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(ctx.llvmCtx, "then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(ctx.llvmCtx, "else", func);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx.llvmCtx, "ifcont", func);
    
    ctx.builder.CreateCondBr(cond, thenBB, elseBB);
    
    // Then branch
    ctx.builder.SetInsertPoint(thenBB);
    lowerStatement(stmt->thenBranch, ctx);
    ctx.builder.CreateBr(mergeBB);
    
    // Else branch
    ctx.builder.SetInsertPoint(elseBB);
    if (stmt->elseBranch) {
        lowerStatement(stmt->elseBranch, ctx);
    }
    ctx.builder.CreateBr(mergeBB);
    
    ctx.builder.SetInsertPoint(mergeBB);
}

void lowerForStmt(const ForStmtAST* stmt, CodeGenContext& ctx) {
    llvm::Function* func = ctx.getCurrentFunction();
    
    llvm::BasicBlock* headerBB = llvm::BasicBlock::Create(ctx.llvmCtx, "loop.header", func);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(ctx.llvmCtx, "loop.body", func);
    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(ctx.llvmCtx, "loop.exit", func);
    
    // Push loop context
    ctx.pushLoop(headerBB, exitBB, bodyBB);
    
    // Branch to header
    ctx.builder.CreateBr(headerBB);
    
    // Header
    ctx.builder.SetInsertPoint(headerBB);
    // For loops have an iterable - simplified for example
    // Actually lower the iterable and loop condition here
    
    // Body
    ctx.builder.SetInsertPoint(bodyBB);
    lowerStatement(stmt->body, ctx);
    ctx.builder.CreateBr(headerBB);  // continue
    
    // Exit
    ctx.builder.SetInsertPoint(exitBB);
    
    ctx.popLoop();
}

} // namespace codegen