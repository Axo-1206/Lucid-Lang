// CodeGenDecl.cpp
#include "CodeGenContext.hpp"

namespace codegen {

void lowerDeclaration(const DeclAST* decl, CodeGenContext& ctx) {
    switch (decl->kind) {
        case ASTKind::FuncDecl:
            lowerFunctionDecl(decl->as<FuncDeclAST>(), ctx);
            break;
        case ASTKind::VarDecl:
            lowerVarDecl(decl->as<VarDeclAST>(), ctx);
            break;
        case ASTKind::StructDecl:
            lowerStructDecl(decl->as<StructDeclAST>(), ctx);
            break;
        case ASTKind::EnumDecl:
            lowerEnumDecl(decl->as<EnumDeclAST>(), ctx);
            break;
        default:
            break;
    }
}

void lowerFunctionDecl(const FuncDeclAST* decl, CodeGenContext& ctx) {
    // Check if foreign
    if (hasForeignAttribute(decl)) {
        lowerForeignFunction(decl, ctx);
        return;
    }
    
    // Create function type
    llvm::FunctionType* funcType = ctx.getFunctionType(decl->funcType);
    if (!funcType) return;
    
    // Create function
    std::string name = StringPool::instance().lookup(decl->name);
    llvm::Function* func = llvm::Function::Create(
        funcType,
        llvm::Function::ExternalLinkage,
        name,
        ctx.module
    );
    
    // Store for later
    ctx.functions[decl] = func;
    
    // Set parameter names
    size_t idx = 0;
    for (auto& arg : func->args()) {
        if (idx < decl->funcType->params.size()) {
            std::string pname = StringPool::instance().lookup(
                decl->funcType->params[idx]->name
            );
            arg.setName(pname);
        }
        idx++;
    }
    
    // Create entry block (will be filled in Phase 2)
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(
        ctx.llvmCtx, 
        "entry", 
        func
    );
}

void lowerFunctionBody(const FuncDeclAST* decl, CodeGenContext& ctx) {
    auto it = ctx.functions.find(decl);
    if (it == ctx.functions.end()) return;
    
    llvm::Function* func = it->second;
    if (func->empty()) return;
    
    ctx.setCurrentFunction(func);
    ctx.builder.SetInsertPoint(&func->front());
    
    // Push scope for parameters
    ctx.pushScope();
    
    // Create allocas for parameters
    size_t idx = 0;
    for (auto& arg : func->args()) {
        if (idx < decl->funcType->params.size()) {
            const ParamAST* param = decl->funcType->params[idx];
            
            llvm::AllocaInst* alloca = ctx.builder.CreateAlloca(
                arg.getType(),
                nullptr,
                StringPool::instance().lookup(param->name)
            );
            ctx.builder.CreateStore(&arg, alloca);
            ctx.insertValue(param, alloca);
        }
        idx++;
    }
    
    // Lower body
    if (decl->body) {
        lowerStatement(decl->body, ctx);
    }
    
    // Add return if missing
    if (func->getReturnType()->isVoidTy()) {
        ctx.builder.CreateRetVoid();
    }
    
    ctx.popScope();
    ctx.setCurrentFunction(nullptr);
}

void lowerVarDecl(const VarDeclAST* decl, CodeGenContext& ctx) {
    llvm::Type* varType = ctx.getType(decl->type);
    if (!varType) return;
    
    std::string name = StringPool::instance().lookup(decl->name);
    llvm::AllocaInst* alloca = ctx.builder.CreateAlloca(varType, nullptr, name);
    ctx.insertValue(decl, alloca);
    
    if (decl->init) {
        llvm::Value* init = lowerExpression(decl->init, ctx);
        if (init) {
            ctx.builder.CreateStore(init, alloca);
        }
    }
}

} // namespace codegen