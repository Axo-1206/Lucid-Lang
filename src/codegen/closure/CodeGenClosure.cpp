/// @file CodeGenClosure.cpp
/// @brief Implementation of closure lowering to LLVM IR.
///
/// ─── Closure Lowering Overview ──────────────────────────────────────────────
/// A closure is a function that captures variables from its enclosing scope.
/// The lowering process consists of three main parts:
///
///   1. ENVIRONMENT STRUCT: An LLVM struct type where each field corresponds
///      to a captured variable. This struct holds the captured state.
///
///   2. CLOSURE FUNCTION: The actual function implementation. It takes the
///      environment pointer as its first argument, followed by the regular
///      parameters. It loads captured values from the environment.
///
///   3. CLOSURE VALUE (FAT POINTER): A struct { function pointer, environment
///      pointer }. This is what's returned and passed around at runtime.
///
/// ─── Memory Management ──────────────────────────────────────────────────────
/// The environment is heap-allocated (via __lucid_alloc_env) and reference-
/// counted. This ensures the closure can outlive the stack frame where it
/// was created (essential for returning closures from functions).
///
/// ─── Capture Handling ──────────────────────────────────────────────────────
/// Two types of captures:
///   - By Value (byReference = false): The value is copied into the environment
///   - By Reference (byReference = true): A pointer to the variable is stored
///
/// By-reference captures require the captured variable to be heap-allocated
/// if the closure may escape. This is handled by Sema (promotion analysis).
///
/// ─── Control Flow ──────────────────────────────────────────────────────────
/// The closure function is verified after generation to ensure:
///   - All basic blocks are properly terminated
///   - Return types match the function signature
///   - No invalid instructions

#include "CodeGenClosure.hpp"
#include "../CodeGen.hpp"
#include "../CodeGenType.hpp"
#include "../generic/CodeGenGeneric.hpp"
#include "../support/CodeGenAlloca.hpp"
#include "../support/CodeGenPanic.hpp"
#include "core/trace/Trace.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <cassert>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>

#include <atomic>
#include <unordered_map>

namespace codegen {

// ─── Static Counter for Unique Closure Names ──────────────────────────────
// Using a static counter ensures unique names across all closures in the
// compilation session, even across different modules.
static std::atomic<size_t> g_closureCounter{0};

// ─── Forward Declarations ───────────────────────────────────────────────────

/// @brief Helper to determine if a capture should be stored by value or reference.
static bool shouldCaptureByValue(const CapturedVariable& capture);

/// @brief Helper to get the actual LLVM type for a captured variable.
static llvm::Type* getCaptureFieldType(CodeGenContext& ctx, const CapturedVariable& capture);

/// @brief Helper to emit the closure function body after environment setup.
static bool emitClosureBody(AnonFuncExprAST* expr, llvm::Function* closureFunc,
                           llvm::Value* envPtr, CodeGenContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Main Entry Point
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* lowerClosure(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    const bool hasCaptures = !expr->captures.empty();

    // ─── 1. Build the closure environment struct ──────────────────────────
    // buildClosureEnvironment already returns a valid (empty) struct type
    // when there are no captures, so this is safe to call unconditionally -
    // every closure value gets the same { funcPtr, envPtr } shape, whether
    // or not it captures anything. A uniform shape matters because
    // lowerCallExpr (CodeGenExpr.cpp) distinguishes "closure value" from
    // "plain named function" purely by checking whether the lowered value
    // is a struct - a non-capturing closure that fell back to a bare
    // pointer would silently be uncallable through that path.
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

    llvm::Value* envPtr = nullptr;

    if (hasCaptures) {
        // ─── 4. Allocate the environment ───────────────────────────────────
        // The environment is heap-allocated (refcounted).
        llvm::Function* allocEnv = ctx.getRuntimeFn(RuntimeFn::AllocEnv);

        // ─── 5. Get the size of the environment ────────────────────────────
        llvm::DataLayout dl(ctx.module);
        uint64_t envSize = dl.getTypeAllocSize(envType);

        // ─── 6. Allocate the environment ───────────────────────────────────
        llvm::Value* envSizeVal = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx.llvmCtx),
            envSize
        );
        envPtr = ctx.builder.CreateCall(allocEnv, {envSizeVal}, "env_ptr");

        // ─── 7. Cast to the environment type ───────────────────────────────
        llvm::Value* typedEnvPtr = ctx.builder.CreatePointerCast(
            envPtr,
            llvm::PointerType::get(envType, 0),
            "typed_env"
        );

        // ─── 8. Fill the environment with captured variables ───────────────
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

            // ─── Handle by-value vs by-reference captures ───────────────────
            if (!capture.byReference) {
                // By value: load the value and store it in the environment
                llvm::Type* declType = getType(ctx, capture.decl->type);
                if (declType) {
                    // Use the proper loadIfNeeded with explicit element type
                    capturedValue = loadIfNeeded(capturedValue, declType, ctx);
                } else {
                    ctx.diagnostics.errorAt(DiagCode::Sem_InvalidCapture, expr->loc,
                                            "captured variable '", 
                                            ctx.pool.lookup(capture.decl->name),
                                            "' has invalid type");
                    continue;
                }
            } else {
                // By reference: store the address (pointer) in the environment
                // The capturedValue should already be a pointer to the variable
                // (either stack or heap allocated). We store this pointer directly.
                // Note: If the variable is on the stack and the closure escapes,
                // this would be unsafe. Sema should have promoted such variables
                // to heap allocation before we get here.
                if (!capturedValue->getType()->isPointerTy()) {
                    ctx.diagnostics.errorAt(DiagCode::Sem_InvalidCapture, expr->loc,
                                            "by-reference capture of '",
                                            ctx.pool.lookup(capture.decl->name),
                                            "' requires a pointer, got non-pointer value");
                    continue;
                }
            }

            // ─── Get the field pointer in the environment ───────────────────
            llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
                envType,
                typedEnvPtr,
                capture.index,
                "env_field_" + ctx.pool.lookup(capture.decl->name)
            );

            // ─── Store the captured value ────────────────────────────────────
            ctx.builder.CreateStore(capturedValue, fieldPtr);
        }
    } else {
        // ─── No captures: skip heap allocation entirely ────────────────────
        // There's nothing to store, so there's no reason to round-trip
        // through __lucid_alloc_env for an empty struct. The closure
        // function still takes an env-pointer first argument (uniform ABI,
        // see createClosureFunction/emitClosureCall), it just never
        // dereferences it, since expr->captures is empty and the
        // capture-loading loop in emitClosureBody does zero iterations.
        envPtr = llvm::ConstantPointerNull::get(
            llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    // ─── 9. Create the closure value (fat pointer) ─────────────────────────
    // A closure is { function pointer, environment pointer }
    // We use a struct { i8*, i8* } for the fat pointer. This is built the
    // same way whether or not there are captures, so every closure value -
    // capturing or not - has the same LLVM shape and can be called through
    // the same path in lowerCallExpr (CodeGenExpr.cpp).
    //
    // IMPORTANT: this must be StructType::get (an anonymous/literal struct
    // type), not StructType::create (a named/identified struct type).
    // StructType::create mints a brand new, distinct type identity on
    // every call - two structurally identical closures built at two
    // different call sites would get different LLVM types (%closure,
    // %closure.1, ...), which LLVM does not unify. StructType::get with
    // no name instead produces a single structurally-uniqued type shared
    // by every closure literal with this shape, which matters as soon as
    // anything needs to treat "a closure value" as one consistent type
    // regardless of where it was created - e.g. a trait field-offset
    // table describing a callable field, or storing different closures
    // into the same variable across branches.
    llvm::StructType* closureType = llvm::StructType::get(
        ctx.llvmCtx,
        {
            llvm::PointerType::get(ctx.llvmCtx, 0),  // function pointer
            llvm::PointerType::get(ctx.llvmCtx, 0)   // environment pointer
        }
    );

    // Build the closure value
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

    // ─── 10. Store the closure value on the AST node ───────────────────────
    expr->llvmValue = closure;

    Trace::detail("Lowered closure with ", expr->captures.size(), " captures");
    return closure;
}

// ─────────────────────────────────────────────────────────────────────────────
// Build Closure Environment
// ─────────────────────────────────────────────────────────────────────────────

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

        // Get the LLVM type for the captured variable
        llvm::Type* fieldType = getCaptureFieldType(ctx, capture);
        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidCapture, expr->loc,
                                    "captured variable '",
                                    ctx.pool.lookup(capture.decl->name),
                                    "' has invalid type");
            // Use a placeholder type to continue
            fieldType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        }

        fieldTypes.push_back(fieldType);
        fieldNames.push_back(ctx.pool.lookup(capture.decl->name));
    }

    // ─── Create the environment struct ────────────────────────────────────
    // Use a unique name with a counter to avoid collisions
    static std::atomic<size_t> envCounter{0};
    std::string envName = "closure_env_" + std::to_string(++envCounter);
    llvm::StructType* envType = llvm::StructType::create(
        ctx.llvmCtx,
        fieldTypes,
        envName
    );

    // ─── Store the environment type on the AST node ──────────────────────
    expr->environmentType = envType;

    Trace::info("Built closure environment with ", fieldTypes.size(), " fields");
    return envType;
}

// ─────────────────────────────────────────────────────────────────────────────
// Create Closure Function
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* createClosureFunction(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── 1. Get the function type ──────────────────────────────────────────
    FuncTypeAST* funcType = expr->funcType;
    if (!funcType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "anonymous function has no type");
        return nullptr;
    }

    // ─── 2. Get or build the environment type ────────────────────────────
    llvm::StructType* envType = expr->environmentType;
    if (!envType) {
        envType = buildClosureEnvironment(expr, ctx);
        if (!envType) return nullptr;
    }

    // ─── 3. Build the function type ──────────────────────────────────────
    // The closure function takes: env pointer + regular parameters
    std::vector<llvm::Type*> paramTypes;
    
    // Environment pointer (typed, not opaque)
    paramTypes.push_back(llvm::PointerType::get(envType, 0));

    // Regular parameters - use GenericSubstitution if needed
    // For closures, we need to handle generic parameters properly
    const GenericSubstitution* subst = nullptr;
    // If the closure is inside a generic context, we need substitution
    // For now, we assume no substitution for anonymous functions
    
    for (ParamAST* param : funcType->params) {
        llvm::Type* paramType = getType(ctx, param->type, subst);
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
        returnType = getType(ctx, funcType->returnType, subst);
    }
    if (!returnType) {
        returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    }

    llvm::FunctionType* fnType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    // ─── 4. Create the closure function with a unique name ──────────────
    std::string funcName = "closure_" + std::to_string(++g_closureCounter);
    llvm::Function* closureFunc = llvm::Function::Create(
        fnType,
        llvm::Function::InternalLinkage,
        funcName,
        ctx.module
    );

    // ─── 5. Set parameter names ──────────────────────────────────────────
    size_t argIndex = 0;
    closureFunc->getArg(argIndex++)->setName("env");

    for (ParamAST* param : funcType->params) {
        if (argIndex < closureFunc->arg_size()) {
            closureFunc->getArg(argIndex)->setName(ctx.pool.lookup(param->name));
            argIndex++;
        }
    }

    // ─── 6. Emit the function body ──────────────────────────────────────
    if (!emitClosureBody(expr, closureFunc, closureFunc->getArg(0), ctx)) {
        return nullptr;
    }

    Trace::detail("Created closure function: ", funcName);
    return closureFunc;
}

// ─────────────────────────────────────────────────────────────────────────────
// Emit Closure Body
// ─────────────────────────────────────────────────────────────────────────────

static bool emitClosureBody(AnonFuncExprAST* expr, llvm::Function* closureFunc,
                           llvm::Value* envPtr, CodeGenContext& ctx) {
    if (!expr || !closureFunc || !envPtr) return false;

    FuncTypeAST* funcType = expr->funcType;
    if (!funcType) return false;

    llvm::StructType* envType = expr->environmentType;
    if (!envType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "closure has no environment type");
        return false;
    }

    // ─── 1. Push function context ────────────────────────────────────────
    ctx.setCurrentFunction(closureFunc);

    // ─── 2. Create entry block ──────────────────────────────────────────
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "entry",
        closureFunc
    );
    ctx.builder.SetInsertPoint(entryBlock);

    // ─── 3. Load captured variables from environment ────────────────────
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

        // Store in the context's value map for use in the closure body
        ctx.storeValue(capture.decl, capturedValue);
    }

    // ─── 4. Lower parameters ────────────────────────────────────────────
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

    // ─── 5. Lower the body ──────────────────────────────────────────────
    if (expr->body) {
        lowerStatement(const_cast<StmtAST*>(expr->body), ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, expr->loc,
                                "anonymous function has no body");
        ctx.setCurrentFunction(nullptr);
        return false;
    }

    // ─── 6. Pop function context ────────────────────────────────────────
    ctx.setCurrentFunction(nullptr);

    // ─── 7. Verify the function ──────────────────────────────────────────
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyFunction(*closureFunc, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, expr->loc,
                                "closure function failed verification: ", error);
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Emit Closure Call
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* emitClosureCall(
    llvm::Value* funcPtr,
    llvm::Value* envPtr,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::Type* returnType,
    CodeGenContext& ctx
) {
    if (!funcPtr || !envPtr) return nullptr;

    // ─── 1. Build the function type from the arguments ────────────────────
    // The closure function signature is: env pointer + user arguments
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0)); // env pointer
    
    // Store argument types
    for (llvm::Value* arg : args) {
        paramTypes.push_back(arg->getType());
    }

    // ─── 2. Return type ─────────────────────────────────────────────────────
    // Supplied by the caller, derived from the call expression's own
    // resolved type (sema already computes this in resolveCallExpr as
    // funcType->returnType) - callers should not have to guess it here.
    if (!returnType) {
        returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    }

    llvm::FunctionType* fnType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    // ─── 3. Cast the function pointer to the correct type ──────────────
    llvm::Value* typedFunc = ctx.builder.CreatePointerCast(
        funcPtr,
        llvm::PointerType::get(fnType, 0),
        "closure_func_cast"
    );

    // ─── 4. Build argument list ──────────────────────────────────────────
    std::vector<llvm::Value*> callArgs;
    callArgs.reserve(1 + args.size());
    callArgs.push_back(envPtr);  // Environment pointer is first
    for (llvm::Value* arg : args) {
        callArgs.push_back(arg);
    }

    // ─── 5. Create the call ──────────────────────────────────────────────
    llvm::Value* result = ctx.builder.CreateCall(
        fnType,        // FunctionType* - the signature
        typedFunc,     // Value* - the function pointer
        callArgs,
        "closure_call"
    );

    return result;
}

// ─── Callable Dispatch ─────────────────────────────────────────────────────

llvm::Value* emitCallableCall(
    llvm::Value* callee,
    llvm::ArrayRef<llvm::Value*> args,
    llvm::FunctionType* fnType,
    CodeGenContext& ctx,
    const std::string& name
) {
    if (!callee || !fnType) return nullptr;

    // ─── 1. Closure value: { funcPtr, envPtr } fat pointer struct ─────────
    if (callee->getType()->isStructTy()) {
        llvm::Value* funcPtr = ctx.builder.CreateExtractValue(callee, 0, name + "_closure_func");
        llvm::Value* envPtr = ctx.builder.CreateExtractValue(callee, 1, name + "_closure_env");
        return emitClosureCall(funcPtr, envPtr, args, fnType->getReturnType(), ctx);
    }

    // ─── 2. Plain named function reference - the common, fast path ─────────
    if (llvm::Function* fn = llvm::dyn_cast<llvm::Function>(callee)) {
        return ctx.builder.CreateCall(fn, args, name);
    }

    // ─── 3. Indirect function pointer (e.g. loaded from a variable) ────────
    // A function value stored in and loaded from a variable is just a bare
    // `ptr`-typed SSA value at this point - dyn_cast<llvm::Function> above
    // reflects the IR node's static C++ class, not what address it
    // dynamically holds, so it always fails here even though the pointer
    // genuinely does refer to a real function. It needs an explicit cast
    // to the expected signature before it can be called.
    if (callee->getType()->isPointerTy()) {
        llvm::Value* casted = ctx.builder.CreatePointerCast(
            callee, llvm::PointerType::get(fnType, 0), name + "_cast");
        return ctx.builder.CreateCall(fnType, casted, args, name);
    }

    // ─── 4. Neither shape ───────────────────────────────────────────────────
    // Sema already guarantees the source expression resolves to a
    // FuncTypeAST, so reaching here means CodeGen and Sema have gone out
    // of sync, not that the user wrote something uncallable.
    assert(false && "callee is neither a closure value, a plain llvm::Function, "
                     "nor an indirect function pointer - Sema should have caught this");
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper Functions
// ─────────────────────────────────────────────────────────────────────────────

static bool shouldCaptureByValue(const CapturedVariable& capture) {
    // By value if not by reference
    return !capture.byReference;
}

static llvm::Type* getCaptureFieldType(CodeGenContext& ctx, const CapturedVariable& capture) {
    if (!capture.decl) return nullptr;

    // Get the base type from the declaration
    llvm::Type* baseType = getType(ctx, capture.decl->type);
    if (!baseType) return nullptr;

    // If by reference, we store a pointer to the variable
    if (capture.byReference) {
        return llvm::PointerType::get(baseType, 0);
    }

    // By value: store the value directly
    return baseType;
}

bool isClosureNeeded(const AnonFuncExprAST* expr) {
    if (!expr) return false;
    // A closure is needed if there are captures or explicitly marked
    return expr->hasClosure || !expr->captures.empty();
}

} // namespace codegen