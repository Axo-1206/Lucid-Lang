// CodeGenExpr.cpp
#include "CodeGenContext.hpp"

namespace codegen {

llvm::Value* lowerExpression(const ExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;
    
    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            return lowerLiteral(expr->as<LiteralExprAST>(), ctx);
        case ASTKind::IdentifierExpr:
            return lowerIdentifier(expr->as<IdentifierExprAST>(), ctx);
        case ASTKind::BinaryExpr:
            return lowerBinary(expr->as<BinaryExprAST>(), ctx);
        case ASTKind::UnaryExpr:
            return lowerUnary(expr->as<UnaryExprAST>(), ctx);
        case ASTKind::CallExpr:
            return lowerCall(expr->as<CallExprAST>(), ctx);
        case ASTKind::IntrinsicCallExpr:
            return lowerIntrinsic(expr->as<IntrinsicCallExprAST>(), ctx);
        case ASTKind::AnonFuncExpr:
            return lowerClosure(expr->as<AnonFuncExprAST>(), ctx);
        default:
            return nullptr;
    }
}

llvm::Value* lowerLiteral(const LiteralExprAST* expr, CodeGenContext& ctx) {
    switch (expr->kind) {
        case LiteralKind::Int:
            return llvm::ConstantInt::get(
                ctx.getType(expr->resolvedType),
                std::stoll(StringPool::instance().lookup(expr->value))
            );
        case LiteralKind::Float:
            return llvm::ConstantFP::get(
                ctx.getType(expr->resolvedType),
                std::stod(StringPool::instance().lookup(expr->value))
            );
        case LiteralKind::True:
            return llvm::ConstantInt::getTrue(ctx.llvmCtx);
        case LiteralKind::False:
            return llvm::ConstantInt::getFalse(ctx.llvmCtx);
        default:
            return nullptr;
    }
}

llvm::Value* lowerIdentifier(const IdentifierExprAST* expr, CodeGenContext& ctx) {
    // Find the declaration
    const ValueDeclAST* decl = /* lookup from scope */ nullptr;
    if (!decl) return nullptr;
    
    llvm::Value* val = ctx.lookupValue(decl);
    if (!val) return nullptr;
    
    // If it's an alloca, load it
    if (llvm::isa<llvm::AllocaInst>(val)) {
        return ctx.builder.CreateLoad(val);
    }
    
    return val;
}

llvm::Value* lowerBinary(const BinaryExprAST* expr, CodeGenContext& ctx) {
    llvm::Value* left = lowerExpression(expr->left, ctx);
    llvm::Value* right = lowerExpression(expr->right, ctx);
    if (!left || !right) return nullptr;
    
    switch (expr->op) {
        case BinaryOp::Add: return ctx.builder.CreateAdd(left, right);
        case BinaryOp::Sub: return ctx.builder.CreateSub(left, right);
        case BinaryOp::Mul: return ctx.builder.CreateMul(left, right);
        case BinaryOp::Div: return ctx.builder.CreateSDiv(left, right);
        case BinaryOp::Eq:  return ctx.builder.CreateICmpEQ(left, right);
        case BinaryOp::Ne:  return ctx.builder.CreateICmpNE(left, right);
        // ... etc
        default: return nullptr;
    }
}

llvm::Value* lowerCall(const CallExprAST* expr, CodeGenContext& ctx) {
    // Resolve callee
    llvm::Value* callee = lowerExpression(expr->callee, ctx);
    if (!callee) return nullptr;
    
    // Lower arguments
    std::vector<llvm::Value*> args;
    for (const ExprPtr arg : expr->args) {
        llvm::Value* val = lowerExpression(arg, ctx);
        if (!val) return nullptr;
        args.push_back(val);
    }
    
    // If callee is a closure, handle specially
    if (expr->callee->isa<AnonFuncExprAST>()) {
        return lowerClosureCall(expr, ctx);
    }
    
    // Regular function call
    return ctx.builder.CreateCall(callee, args);
}

llvm::Value* lowerIntrinsic(const IntrinsicCallExprAST* expr, CodeGenContext& ctx) {
    std::string name = StringPool::instance().lookup(expr->intrinsicName);
    
    // ─── LLVM Intrinsics ──────────────────────────────────────────────
    if (name == "sqrt") {
        llvm::Value* arg = lowerExpression(expr->args[0], ctx);
        if (!arg) return nullptr;
        return ctx.builder.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, arg);
    }
    
    if (name == "memcpy") {
        // ... handle memcpy
        return nullptr;
    }
    
    // ─── Runtime Calls ─────────────────────────────────────────────────
    if (name == "alloc") {
        // ... handle alloc
        return nullptr;
    }
    
    // ─── Compile-Time ──────────────────────────────────────────────────
    if (name == "sizeof") {
        const TypeAST* type = /* get type from expr */ nullptr;
        uint64_t size = ctx.module->getDataLayout().getTypeAllocSize(
            ctx.getType(type)
        );
        return llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx.llvmCtx),
            size
        );
    }
    
    // ─── Control Flow ──────────────────────────────────────────────────
    if (name == "scope_exit") {
        // Register callback for block exit
        // (implemented in CodeGenClosure.cpp)
        return lowerScopeExit(expr, ctx);
    }
    
    return nullptr;
}

} // namespace codegen