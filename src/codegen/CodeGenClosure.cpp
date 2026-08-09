// CodeGenClosure.cpp
#include "context/CodeGenContext.hpp"

namespace codegen {

/// @brief Lower an anonymous function (closure).
llvm::Value* lowerClosure(const AnonFuncExprAST* expr, CodeGenContext& ctx) {
    // ─── 1. Analyze captures ──────────────────────────────────────────────
    std::vector<const ValueDeclAST*> captures = analyzeCaptures(expr, ctx);
    bool hasCaptures = !captures.empty();
    
    // ─── 2. Create closure function ──────────────────────────────────────
    // The closure function takes environment as first argument
    llvm::Function* closureFunc = createClosureFunction(expr, captures, ctx);
    if (!closureFunc) return nullptr;
    
    // ─── 3. Allocate environment (if has captures) ──────────────────────
    llvm::Value* envPtr = nullptr;
    if (hasCaptures) {
        envPtr = allocateClosureEnvironment(expr, captures, ctx);
        if (!envPtr) return nullptr;
        
        // Store captures in environment
        storeCaptures(expr, captures, envPtr, ctx);
    }
    
    // ─── 4. Return fat pointer or raw function pointer ──────────────────
    if (!hasCaptures) {
        // Non-capturing closure - just return function pointer
        return closureFunc;
    }
    
    // Fat pointer: { function_ptr, env_ptr }
    llvm::StructType* closureType = llvm::StructType::create(
        ctx.llvmCtx,
        {
            llvm::PointerType::get(closureFunc->getType(), 0),
            llvm::PointerType::get(envPtr->getType(), 0)
        },
        "closure"
    );
    
    llvm::Value* closure = llvm::ConstantStruct::get(closureType, {closureFunc, envPtr});
    return closure;
}

/// @brief Analyze which variables are captured.
std::vector<const ValueDeclAST*> analyzeCaptures(
    const AnonFuncExprAST* expr, 
    CodeGenContext& ctx
) {
    // Walk the body to find identifier references
    // Collect only those from outer scopes (not parameters of this closure)
    // This is a simplified version - actual implementation walks the AST
    
    std::vector<const ValueDeclAST*> captures;
    // ... capture analysis logic ...
    return captures;
}

/// @brief Create the closure function.
llvm::Function* createClosureFunction(
    const AnonFuncExprAST* expr,
    const std::vector<const ValueDeclAST*>& captures,
    CodeGenContext& ctx
) {
    // Get function type
    llvm::FunctionType* funcType = ctx.getFunctionType(expr->funcType);
    if (!funcType) return nullptr;
    
    // Add environment parameter
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0)); // env
    for (llvm::Type* param : funcType->params()) {
        paramTypes.push_back(param);
    }
    
    llvm::FunctionType* closureFuncType = llvm::FunctionType::get(
        funcType->getReturnType(),
        paramTypes,
        false
    );
    
    // Create function
    std::string name = "closure." + std::to_string(expr->loc.line);
    llvm::Function* func = llvm::Function::Create(
        closureFuncType,
        llvm::Function::InternalLinkage,
        name,
        ctx.module
    );
    
    // Create entry block
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx.llvmCtx, "entry", func);
    ctx.builder.SetInsertPoint(entry);
    
    // Get environment pointer
    llvm::Value* envPtr = &func->args()[0];
    envPtr->setName("env");
    
    // Push scope
    ctx.pushScope();
    
    // Load captures from environment
    for (size_t i = 0; i < captures.size(); ++i) {
        const ValueDeclAST* capture = captures[i];
        int fieldIdx = 1 + i; // field 0 is refcount
        
        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
            envPtr->getType()->getPointerElementType(),
            envPtr,
            fieldIdx
        );
        llvm::Value* captured = ctx.builder.CreateLoad(fieldPtr);
        
        // Store in local alloca
        std::string name = StringPool::instance().lookup(capture->name);
        llvm::AllocaInst* alloca = ctx.builder.CreateAlloca(
            captured->getType(),
            nullptr,
            name
        );
        ctx.builder.CreateStore(captured, alloca);
        ctx.insertValue(capture, alloca);
    }
    
    // Lower parameters (skip env)
    // ... parameter lowering logic ...
    
    // Lower body
    lowerStatement(expr->body, ctx);
    
    // Add return if missing
    if (funcType->getReturnType()->isVoidTy()) {
        ctx.builder.CreateRetVoid();
    }
    
    ctx.popScope();
    return func;
}

/// @brief Allocate closure environment.
llvm::Value* allocateClosureEnvironment(
    const AnonFuncExprAST* expr,
    const std::vector<const ValueDeclAST*>& captures,
    CodeGenContext& ctx
) {
    // Create environment type
    std::vector<llvm::Type*> fields;
    fields.push_back(llvm::Type::getInt64Ty(ctx.llvmCtx)); // refcount
    
    for (const ValueDeclAST* capture : captures) {
        fields.push_back(ctx.getType(capture->type));
    }
    
    std::string envName = "closure_env." + std::to_string(expr->loc.line);
    llvm::StructType* envType = llvm::StructType::create(ctx.llvmCtx, fields, envName);
    
    // Allocate on heap
    // Use runtime function lucid_alloc_refcounted(size)
    llvm::Function* allocFunc = getRuntimeFunction("lucid_alloc_refcounted", ctx);
    
    uint64_t size = ctx.module->getDataLayout().getTypeAllocSize(envType);
    llvm::Value* sizeVal = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(ctx.llvmCtx),
        size
    );
    
    llvm::Value* envPtr = ctx.builder.CreateCall(allocFunc, {sizeVal});
    
    // Set refcount to 1
    llvm::Value* refPtr = ctx.builder.CreateStructGEP(envType, envPtr, 0);
    ctx.builder.CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.llvmCtx), 1),
        refPtr
    );
    
    return envPtr;
}

/// @brief Store captures in environment.
void storeCaptures(
    const AnonFuncExprAST* expr,
    const std::vector<const ValueDeclAST*>& captures,
    llvm::Value* envPtr,
    CodeGenContext& ctx
) {
    llvm::Type* envType = envPtr->getType()->getPointerElementType();
    
    for (size_t i = 0; i < captures.size(); ++i) {
        const ValueDeclAST* capture = captures[i];
        
        // Get the capture value
        llvm::Value* value = ctx.lookupValue(capture);
        if (!value) continue;
        
        // If it's an alloca, load it
        if (llvm::isa<llvm::AllocaInst>(value)) {
            value = ctx.builder.CreateLoad(value);
        }
        
        // Store in environment
        int fieldIdx = 1 + i;
        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(envType, envPtr, fieldIdx);
        ctx.builder.CreateStore(value, fieldPtr);
    }
}

} // namespace codegen