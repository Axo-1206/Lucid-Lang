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
/// ─── Function-Typed Captures ───────────────────────────────────────────────
/// When a captured value has function type (FuncTypeAST), it could be either:
///   - A plain function pointer (1 word)
///   - A closure { func, env } (2 words)
///
/// If isClosureValue is true, we know it's a closure at compile time.
/// If isClosureValue is false, it's a plain function.
/// For parameters/fields where we don't know, Sema sets isClosureValue = true
/// conservatively, and CodeGen emits runtime checks.

#include "CodeGenClosure.hpp"
#include "codegen/CodeGen.hpp"
#include "codegen/support/CodeGenAlloca.hpp"
#include "codegen/support/CodeGenOwnership.hpp"
#include "codegen/support/CodeGenPanic.hpp"
#include "core/SourceLocation.hpp"
#include "core/trace/Trace.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Verifier.h>

#include <cassert>
#include <llvm/IR/IRBuilder.h>

#include <atomic>
#include <unordered_map>

namespace codegen {

// ─── Static Counter for Unique Closure Names ──────────────────────────────
static std::atomic<size_t> g_closureCounter{0};

// ─── Forward Declarations ───────────────────────────────────────────────────

/// @brief Helper to emit the closure function body after environment setup.
static bool emitClosureBody(AnonFuncExprAST* expr, llvm::Function* closureFunc,
                           llvm::Value* envPtr, CodeGenContext& ctx);

                           static llvm::Value* resolveCaptureValue(
    const CapturedVariable& capture, CodeGenContext& ctx)
{
    if (!capture.resolvedDecl) return nullptr;
    return ctx.lookupValue(capture.resolvedDecl);
}

static TypeAST* resolveCaptureType(
    const CapturedVariable& capture, CodeGenContext& ctx)
{
    (void)ctx;
    return capture.resolvedDecl ? capture.resolvedDecl->type : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Closure-shape normalization helpers
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* normalizeToClosureType(llvm::Value* value, CodeGenContext& ctx) {
    if (!value) return nullptr;

    // ─── If it's already a closure type, return as-is ─────────────────────
    if (value->getType()->isStructTy()) {
        return value;
    }

    // ─── If it's a function pointer, wrap it as { func, null } ────────────
    llvm::Type* closureType = ctx.getClosureType();
    llvm::Value* result = llvm::UndefValue::get(closureType);

    // Cast function pointer to i8*
    llvm::Value* funcPtr = value;
    if (funcPtr->getType() != llvm::PointerType::get(ctx.llvmCtx, 0)) {
        funcPtr = ctx.builder.CreatePointerCast(
            funcPtr,
            llvm::PointerType::get(ctx.llvmCtx, 0),
            "closure_func_cast"
        );
    }

    result = ctx.builder.CreateInsertValue(result, funcPtr, 0);
    result = ctx.builder.CreateInsertValue(
        result,
        llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx.llvmCtx, 0)),
        1
    );

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Capture Field Type
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* getCaptureFieldType(CodeGenContext& ctx, const CapturedVariable& capture) {
    if (!capture.resolvedDecl || !capture.resolvedDecl->type) return nullptr;
    return getType(ctx, capture.resolvedDecl->type);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Entry Point
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* lowerClosure(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::StructType* envType = buildClosureEnvironment(expr, ctx);
    if (!envType) return nullptr;

    llvm::Function* closureFunc = createClosureFunction(expr, ctx);
    if (!closureFunc) return nullptr;

    expr->closureFunction = closureFunc;
    expr->environmentType = envType;

    const bool hasCaptures = !expr->captures.empty();
    llvm::Value* envPtr = nullptr;

    if (hasCaptures) {
        llvm::Function* allocEnv = ctx.getRuntimeFn(RuntimeFn::AllocEnv);
        llvm::DataLayout dl(ctx.module);
        uint64_t envSize = dl.getTypeAllocSize(envType);
        llvm::Value* envSizeVal = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx.llvmCtx), envSize);
        envPtr = ctx.builder.CreateCall(allocEnv, {envSizeVal}, "env_ptr");
        envPtr = ctx.builder.CreatePointerCast(
            envPtr, llvm::PointerType::get(envType, 0), "typed_env");

        for (const CapturedVariable& capture : expr->captures) {
            llvm::Value* binding = resolveCaptureValue(capture, ctx);
            TypeAST* capturedType = resolveCaptureType(capture, ctx);

            if (!binding || !capturedType) {
                ctx.diagnostics.errorAt(DiagCode::Sem_InvalidCapture, expr->loc,
                    "captured variable '", ctx.pool.lookup(capture.name),
                    "' could not be resolved");
                return nullptr;
            }

            llvm::Type* fieldType = getType(ctx, capturedType);
            if (!fieldType) return nullptr;

            llvm::Value* storedValue = nullptr;

            if (!capture.byReference && isOwnedBufferType(capturedType)) {
                ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, expr->loc,
                    "capturing an owned buffer (string or dynamic array) "
                    "by value is not yet supported");
                return nullptr;
            }

            if (capture.byReference) {
                if (!binding->getType()->isPointerTy()) {
                    ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, expr->loc,
                        "by-reference capture '", ctx.pool.lookup(capture.name),
                        "' did not resolve to a pointer");
                    return nullptr;
                }
                storedValue = binding;
            } else {
                if (llvm::isa<llvm::AllocaInst>(binding)
                    || llvm::isa<llvm::GlobalVariable>(binding)) {
                    storedValue = ctx.builder.CreateLoad(
                        fieldType, binding,
                        "capture_" + ctx.pool.lookup(capture.name));
                } else {
                    storedValue = binding;
                }
            }

            if (!capture.byReference && capturedType->isa<FuncTypeAST>()) {
                if (!storedValue->getType()->isStructTy()) {
                    storedValue = normalizeToClosureType(storedValue, ctx);
                }
                llvm::Value* envForRetain = ctx.builder.CreateExtractValue(
                    storedValue, 1, "capture_env");
                llvm::Function* retainFn = ctx.getRuntimeFn(RuntimeFn::RetainEnv);
                ctx.builder.CreateCall(retainFn, {envForRetain});
            }

            llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
                envType, envPtr, capture.index,
                "env_field_" + ctx.pool.lookup(capture.name));
            ctx.builder.CreateStore(storedValue, fieldPtr);
        }
    } else {
        envPtr = llvm::ConstantPointerNull::get(
            llvm::PointerType::get(ctx.llvmCtx, 0));
    }

    llvm::StructType* closureType = ctx.getClosureType();
    llvm::Value* closure = llvm::UndefValue::get(closureType);
    closure = ctx.builder.CreateInsertValue(
        closure,
        ctx.builder.CreatePointerCast(closureFunc,
            llvm::PointerType::get(ctx.llvmCtx, 0)),
        0);
    closure = ctx.builder.CreateInsertValue(closure, envPtr, 1);
    expr->llvmValue = closure;
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
        if (!capture.name.isValid()) continue;

        // Get the LLVM type for the captured variable
        llvm::Type* fieldType = getCaptureFieldType(ctx, capture);
        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidCapture, expr->loc,
                                    "captured variable '",
                                    ctx.pool.lookup(capture.name),
                                    "' has invalid type");
            // Use a placeholder type to continue
            fieldType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        }

        fieldTypes.push_back(fieldType);
        fieldNames.push_back(ctx.pool.lookup(capture.name));
    }

    // ─── Create the environment struct ────────────────────────────────────
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
    // We use getFunctionType with isClosure = true to add the env parameter
    llvm::FunctionType* fnType = getFunctionType(ctx, funcType, true);
    if (!fnType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "failed to build function type for closure");
        return nullptr;
    }

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

    // ─── 1. Save the previous function context ──────────────────────────
    llvm::Function* prevFunc = ctx.currentFunction;
    llvm::Value* prevEnv = ctx.currentEnvPtr;
    TypeAST* prevReturnType = ctx.currentDeclaredReturnType;

    ctx.setCurrentFunction(closureFunc);
    ctx.currentDeclaredReturnType = funcType->returnType;

    // ─── 2. Create entry block ──────────────────────────────────────────
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx, "entry", closureFunc);
    ctx.builder.SetInsertPoint(entryBlock);

    // ─── 3. Load captured values from the environment ───────────────────
    // This runs FIRST, before parameters and body, because the body's
    // identifier expressions resolve against `ctx.values` — the same map
    // these loads populate. Populating it after body lowering (as an
    // earlier revision did) left every captured name unresolved at the
    // point the body actually needed it.
    //
    // By-reference captures: the env field holds a pointer to the
    //   captured binding's storage. Store that pointer directly under
    //   `resolvedDecl`; identifier loads through it see the live value.
    // By-value captures: the env field holds a copy. Spill it into a
    //   fresh alloca so mutations inside the closure do not reach the
    //   original, and store the alloca under `resolvedDecl`.
    for (size_t i = 0; i < expr->captures.size(); ++i) {
        const CapturedVariable& capture = expr->captures[i];
        if (!capture.name.isValid()) continue;

        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
            envType, envPtr, i,
            "captured_" + ctx.pool.lookup(capture.name));
        llvm::Type* fieldType = envType->getElementType(i);
        llvm::Value* capturedValue = ctx.builder.CreateLoad(
            fieldType, fieldPtr,
            "load_captured_" + ctx.pool.lookup(capture.name));

        if (capture.byReference) {
            if (!capture.resolvedDecl) continue;
            ctx.storeValue(capture.resolvedDecl, capturedValue);
        } else {
            llvm::AllocaInst* spill = ctx.builder.CreateAlloca(
                fieldType, nullptr,
                "capture_spill_" + ctx.pool.lookup(capture.name));
            ctx.builder.CreateStore(capturedValue, spill);
            if (capture.resolvedDecl) {
                ctx.storeValue(capture.resolvedDecl, spill);
            }
        }
    }

    // ─── 4. Lower parameters ────────────────────────────────────────────
    size_t paramArgIndex = 1;
    for (ParamAST* param : funcType->params) {
        if (paramArgIndex < closureFunc->arg_size()) {
            llvm::Value* argValue = closureFunc->getArg(paramArgIndex);
            paramArgIndex++;

            llvm::Type* paramType = getType(ctx, param->type);
            if (paramType) {
                llvm::AllocaInst* alloca = createAlloca(
                    ctx.pool.lookup(param->name), paramType, ctx);
                ctx.builder.CreateStore(argValue, alloca);
                ctx.storeValue(param, alloca);
                param->llvmAlloca = alloca;
                param->llvmValue = argValue;

                if (ownsResource(param)) {
                    ctx.markAlive(param);
                }
            }
        }
    }

    // ─── 5. Lower the body ──────────────────────────────────────────────
    if (expr->body) {
        lowerStatement(expr->body, ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, expr->loc,
                                "anonymous function has no body");
        ctx.currentFunction = prevFunc;
        ctx.currentEnvPtr = prevEnv;
        ctx.currentDeclaredReturnType = prevReturnType;
        return false;
    }

    ctx.currentFunction = prevFunc;
    ctx.currentEnvPtr = prevEnv;
    ctx.currentDeclaredReturnType = prevReturnType;

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
    std::vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(ctx.llvmCtx, 0)); // env pointer
    
    for (llvm::Value* arg : args) {
        paramTypes.push_back(arg->getType());
    }

    if (!returnType) {
        returnType = llvm::Type::getVoidTy(ctx.llvmCtx);
    }

    llvm::FunctionType* fnType = llvm::FunctionType::get(
        returnType,
        paramTypes,
        false
    );

    // ─── 2. Cast the function pointer to the correct type ──────────────
    llvm::Value* typedFunc = ctx.builder.CreatePointerCast(
        funcPtr,
        llvm::PointerType::get(fnType, 0),
        "closure_func_cast"
    );

    // ─── 3. Build argument list ──────────────────────────────────────────
    std::vector<llvm::Value*> callArgs;
    callArgs.reserve(1 + args.size());
    callArgs.push_back(envPtr);
    for (llvm::Value* arg : args) {
        callArgs.push_back(arg);
    }

    // ─── 4. Create the call ──────────────────────────────────────────────
    llvm::Value* result = ctx.builder.CreateCall(
        fnType,
        typedFunc,
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
    FuncShape shape,
    CodeGenContext& ctx,
    const std::string& name)
{
    if (!callee || !fnType) return nullptr;

    if (shape == FuncShape::Fn) {
        // Bare function pointer (or llvm::Function*).
        // Cast to the expected signature, call directly.
        llvm::Value* typed = callee;
        if (callee->getType() != llvm::PointerType::get(fnType, 0)) {
            typed = ctx.builder.CreatePointerCast(
                callee,
                llvm::PointerType::get(fnType, 0),
                name + "_fn_cast");
        }
        return ctx.builder.CreateCall(fnType, typed, args, name);
    }

    // shape == FuncShape::Cls
    // Fat pointer { func, env }. Extract both, prepend env.
    llvm::Value* funcPtr = ctx.builder.CreateExtractValue(
        callee, 0, name + "_func");
    llvm::Value* envPtr = ctx.builder.CreateExtractValue(
        callee, 1, name + "_env");
    return emitClosureCall(funcPtr, envPtr, args,
                           fnType->getReturnType(), ctx);
}


// ─────────────────────────────────────────────────────────────────────────────
// Helper Functions
// ─────────────────────────────────────────────────────────────────────────────

bool isClosureNeeded(const AnonFuncExprAST* expr) {
    if (!expr) return false;
    return expr->hasClosure || !expr->captures.empty();
}

} // namespace codegen