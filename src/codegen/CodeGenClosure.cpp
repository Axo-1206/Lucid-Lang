/// @file CodeGenClosure.cpp
/// @brief Implementation of closure lowering to LLVM IR.

#include "CodeGen.hpp"
#include "CodeGenType.hpp"
#include "support/CodeGenAlloca.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>

#include <unordered_map>

namespace codegen {

// =============================================================================
// Closure Lowering - Main Entry Point
// =============================================================================

llvm::Value* lowerClosure(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── 1. Build the closure environment struct ──────────────────────────
    llvm::StructType* envType = buildClosureEnvironment(expr, ctx);
    if (!envType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "failed to build closure environment");
        return nullptr;
    }

    // ─── 2. Create the closure function ───────────────────────────────────
    llvm::Function* closureFunc = createClosureFunction(expr, ctx);
    if (!closureFunc) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "failed to create closure function");
        return nullptr;
    }

    // ─── 3. Store the closure function on the AST node ────────────────────
    expr->closureFunction = closureFunc;
    expr->environmentType = envType;

    // ─── 4. Allocate the environment ──────────────────────────────────────
    // The environment is heap-allocated (refcounted)
    // We call a runtime function to allocate the environment
    llvm::Function* allocEnv = ctx.getRuntimeFunction("__lucid_alloc_env");
    if (!allocEnv) {
        // Declare the alloc_env function
        // void* __lucid_alloc_env(uint64_t size)
        llvm::FunctionType* allocType = llvm::FunctionType::get(
            llvm::PointerType::get(ctx.llvmCtx, 0),
            {llvm::Type::getInt64Ty(ctx.llvmCtx)},
            false
        );
        allocEnv = llvm::Function::Create(
            allocType,
            llvm::Function::ExternalLinkage,
            "__lucid_alloc_env",
            ctx.module
        );
        ctx.setRuntimeFunction("__lucid_alloc_env", allocEnv);
    }

    // ─── 5. Get the size of the environment ──────────────────────────────
    llvm::DataLayout dl(ctx.module);
    uint64_t envSize = dl.getTypeAllocSize(envType);

    // ─── 6. Allocate the environment ──────────────────────────────────────
    llvm::Value* envSizeVal = llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(ctx.llvmCtx),
        envSize
    );
    llvm::Value* envPtr = ctx.builder.CreateCall(allocEnv, {envSizeVal}, "env_ptr");

    // ─── 7. Cast to the environment type ──────────────────────────────────
    llvm::Value* typedEnvPtr = ctx.builder.CreatePointerCast(
        envPtr,
        llvm::PointerType::get(envType, 0),
        "typed_env"
    );

    // ─── 8. Fill the environment with captured variables ──────────────────
    // For each captured variable, store its value into the environment
    for (const CapturedVariable& capture : expr->captures) {
        if (!capture.decl) continue;

        // Look up the captured variable's value
        llvm::Value* capturedValue = ctx.lookupValue(capture.decl);
        if (!capturedValue) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidCapture, expr->loc,
                                    "captured variable '", 
                                    ctx.pool.lookup(capture.decl->name),
                                    "' has no LLVM value");
            continue;
        }

        // If it's an l-value, load the value
        if (capture.byReference) {
            // By reference: we want the address to share mutations
            // The capturedValue should already be a pointer
            // Store the pointer in the environment
        } else {
            // By value: load the value and store it
            capturedValue = loadIfNeeded(capturedValue, true, ctx);
        }

        // Get the field pointer in the environment
        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
            envType,
            typedEnvPtr,
            capture.index,
            "env_field_" + ctx.pool.lookup(capture.decl->name)
        );

        // Store the captured value
        if (capture.byReference) {
            // Store the pointer (address)
            ctx.builder.CreateStore(capturedValue, fieldPtr);
        } else {
            // Store the value
            ctx.builder.CreateStore(capturedValue, fieldPtr);
        }
    }

    // ─── 9. Create the closure value (fat pointer) ───────────────────────
    // A closure is { function pointer, environment pointer }
    // For now, we return a struct { i8*, i8* }
    llvm::StructType* closureType = llvm::StructType::create(
        ctx.llvmCtx,
        {
            llvm::PointerType::get(ctx.llvmCtx, 0),  // function pointer
            llvm::PointerType::get(ctx.llvmCtx, 0)   // environment pointer
        },
        "closure"
    );

    llvm::Value* closure = llvm::UndefValue::get(closureType);
    closure = ctx.builder.CreateInsertValue(
        closure,
        ctx.builder.CreatePointerCast(closureFunc, llvm::PointerType::get(ctx.llvmCtx, 0)),
        0
    );
    closure = ctx.builder.CreateInsertValue(
        closure,
        envPtr,
        1
    );

    // ─── 10. Store the closure value on the AST node ──────────────────────
    expr->llvmValue = closure;

    LOG_CODEGEN("Lowered closure with ", expr->captures.size(), " captures");
    return closure;
}

// =============================================================================
// Build Closure Environment
// =============================================================================

llvm::StructType* buildClosureEnvironment(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── If no captures, return an empty struct ──────────────────────────
    if (expr->captures.empty()) {
        return llvm::StructType::create(ctx.llvmCtx, "closure_env_empty");
    }

    // ─── Build field types for each captured variable ─────────────────────
    std::vector<llvm::Type*> fieldTypes;
    std::vector<std::string> fieldNames;

    for (const CapturedVariable& capture : expr->captures) {
        if (!capture.decl) continue;

        // Get the type of the captured variable
        llvm::Type* fieldType = getType(ctx, capture.decl->type);
        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidCapture, expr->loc,
                                    "captured variable '",
                                    ctx.pool.lookup(capture.decl->name),
                                    "' has invalid type");
            fieldType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        }

        // By reference: store a pointer to the variable
        if (capture.byReference) {
            fieldType = llvm::PointerType::get(fieldType, 0);
        }

        fieldTypes.push_back(fieldType);
        fieldNames.push_back(ctx.pool.lookup(capture.decl->name));
    }

    // ─── Create the environment struct ────────────────────────────────────
    std::string envName = "closure_env_" + std::to_string(expr->captures.size());
    llvm::StructType* envType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        envName
    );

    // ─── Store the environment type on the AST node ──────────────────────
    expr->environmentType = envType;

    LOG_CODEGEN_DETAIL("Built closure environment with ", fieldTypes.size(), " fields");
    return envType;
}

// =============================================================================
// Create Closure Function
// =============================================================================

llvm::Function* createClosureFunction(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Get the function type ────────────────────────────────────────────
    const FuncTypeAST* funcType = expr->funcType;
    if (!funcType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "anonymous function has no type");
        return nullptr;
    }

    // ─── Get the environment type ─────────────────────────────────────────
    llvm::StructType* envType = expr->environmentType;
    if (!envType) {
        envType = buildClosureEnvironment(expr, ctx);
        if (!envType) return nullptr;
    }

    // ─── Build the function type (with environment pointer as first arg) ──
    // The closure function takes the environment pointer as its first argument
    std::vector<llvm::Type*> paramTypes;
    
    // Environment pointer
    paramTypes.push_back(llvm::PointerType::get(envType, 0));

    // Regular parameters
    for (const ParamAST* param : funcType->params) {
        llvm::Type* paramType = getType(ctx, param->type);
        if (!paramType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
                                    "parameter '", ctx.pool.lookup(param->name),
                                    "' has invalid type");
            return nullptr;
        }
        paramTypes.push_back(paramType);
    }

    // Return type
    llvm::Type* returnType = nullptr;
    if (funcType->returnType) {
        returnType = getType(ctx, funcType->returnType);
    }
    if (!returnType) {
        returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    }

    llvm::FunctionType* fnType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    // ─── Create the closure function ──────────────────────────────────────
    std::string funcName = "closure_" + std::to_string(expr->loc.line());
    llvm::Function* closureFunc = llvm::Function::Create(
        fnType,
        llvm::Function::InternalLinkage,
        funcName,
        ctx.module
    );

    // ─── Set parameter names ──────────────────────────────────────────────
    size_t argIndex = 0;
    closureFunc->getArg(argIndex++)->setName("env");

    for (ParamAST* param : funcType->params) {
        if (argIndex < closureFunc->arg_size()) {
            closureFunc->getArg(argIndex)->setName(ctx.pool.lookup(param->name));
            argIndex++;
        }
    }

    // ─── Push function context ────────────────────────────────────────────
    ctx.setCurrentFunction(closureFunc);

    // ─── Create entry block ──────────────────────────────────────────────
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "entry",
        closureFunc
    );
    ctx.builder.SetInsertPoint(entryBlock);

    // ─── Get the environment pointer ──────────────────────────────────────
    llvm::Value* envPtr = closureFunc->getArg(0);

    // ─── Load captured variables from the environment ─────────────────────
    // Store them in the context's value map for use in the body
    for (size_t i = 0; i < expr->captures.size(); ++i) {
        const CapturedVariable& capture = expr->captures[i];
        if (!capture.decl) continue;

        // Get the field pointer from the environment
        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
            envType,
            envPtr,
            i,
            "captured_" + ctx.pool.lookup(capture.decl->name)
        );

        // Load the captured value
        llvm::Type* fieldType = envType->getElementType(i);
        llvm::Value* capturedValue = ctx.builder.CreateLoad(
            fieldType,
            fieldPtr,
            "load_captured_" + ctx.pool.lookup(capture.decl->name)
        );

        // Store in the context's value map
        ctx.storeValue(capture.decl, capturedValue);
    }

    // ─── Lower parameters ─────────────────────────────────────────────────
    // Parameters are arguments after the environment pointer
    size_t paramArgIndex = 1;
    for (ParamAST* param : funcType->params) {
        if (paramArgIndex < closureFunc->arg_size()) {
            llvm::Value* argValue = closureFunc->getArg(paramArgIndex);
            paramArgIndex++;

            // Create alloca for the parameter
            llvm::Type* paramType = getType(ctx, param->type);
            if (paramType) {
                llvm::AllocaInst* alloca = createAlloca(
                    ctx.pool.lookup(param->name),
                    paramType,
                    ctx
                );
                ctx.builder.CreateStore(argValue, alloca);
                ctx.storeValue(param, alloca);
                param->llvmAlloca = alloca;
                param->llvmValue = argValue;
            }
        }
    }

    // ─── Lower the body ───────────────────────────────────────────────────
    if (expr->body) {
        lowerStatement(const_cast<StmtAST*>(expr->body), ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, expr->loc,
                                "anonymous function has no body");
    }

    // ─── Pop function context ─────────────────────────────────────────────
    ctx.setCurrentFunction(nullptr);

    // ─── Verify the function ──────────────────────────────────────────────
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyFunction(*closureFunc, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, expr->loc,
                                "closure function failed verification: ", error);
        return nullptr;
    }

    LOG_CODEGEN("Created closure function: ", funcName);
    return closureFunc;
}

// =============================================================================
// Emit Closure Call
// =============================================================================

llvm::Value* emitClosureCall(
    llvm::Value* funcPtr,
    llvm::Value* envPtr,
    llvm::ArrayRef<llvm::Value*> args,
    CodeGenContext& ctx
) {
    if (!funcPtr || !envPtr) return nullptr;

    // ─── Build the function type ──────────────────────────────────────
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0)); // env pointer
    for (llvm::Value* arg : args) {
        paramTypes.push_back(arg->getType());
    }

    // Determine return type - assume the first argument's type or void
    llvm::Type* returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    if (!args.empty()) {
        // The return type depends on the function being called
        // We'll use the type from the function pointer
        // For now, assume the return type matches the last argument's type
        // In practice, this should come from the AST
        returnType = args[0]->getType();
    }

    llvm::FunctionType* fnType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    // ─── Cast the function pointer to the correct type ──────────────
    llvm::Value* typedFunc = ctx.builder.CreatePointerCast(
        funcPtr,
        llvm::PointerType::get(fnType, 0),
        "closure_func_cast"
    );

    // ─── Build argument list ──────────────────────────────────────────
    std::vector<llvm::Value*> callArgs;
    callArgs.push_back(envPtr);  // Environment pointer is first
    for (llvm::Value* arg : args) {
        callArgs.push_back(arg);
    }

    // ─── Create the call using FunctionType* + Value* overload ─────
    // This overload takes (FunctionType*, Value*, ArrayRef<Value*>, ...)
    llvm::Value* result = ctx.builder.CreateCall(
        fnType,        // FunctionType* - the signature
        typedFunc,     // Value* - the function pointer
        callArgs,
        "closure_call"
    );

    return result;
}

// =============================================================================
// Helper: Check if a closure is needed
// =============================================================================

bool isClosureNeeded(const AnonFuncExprAST* expr) {
    if (!expr) return false;
    return expr->hasClosure || !expr->captures.empty();
}

} // namespace codegen