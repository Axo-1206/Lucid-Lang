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

// ─────────────────────────────────────────────────────────────────────────────
// Runtime Closure Check Helpers
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* emitIsClosureCheck(llvm::Value* value, CodeGenContext& ctx) {
    if (!value) return nullptr;

    // ─── If the value is already a struct, it's a closure ─────────────────
    // The closure type is { ptr, ptr }. If we see this, return true.
    if (value->getType()->isStructTy()) {
        return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 1);
    }

    // ─── If it's a function pointer, it's not a closure ──────────────────
    if (llvm::isa<llvm::Function>(value)) {
        return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 0);
    }

    // ─── For pointer values, use runtime check ────────────────────────────
    if (value->getType()->isPointerTy()) {
        // Use __lucid_is_closure(ptr) -> i1
        llvm::Function* isClosureFn = ctx.getRuntimeFn(RuntimeFn::IsClosure);
        if (!isClosureFn) {
            // Fallback: assume it's not a closure
            return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 0);
        }
        return ctx.builder.CreateCall(isClosureFn, {value}, "is_closure");
    }

    // ─── Unknown value type ──────────────────────────────────────────────
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 0);
}

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
    // ─── TODO: Scope walker not yet implemented ─────────────────────────
    // Under the redesign, a capture identifies its declaration by
    // (name, functionDepth), not by a ValueDeclAST* pointer. Resolving
    // the type requires walking ctx.stack's function frames up to
    // functionDepth and looking up `name` in the enclosing function's
    // scope map. That infrastructure doesn't exist yet.
    //
    // Until it does, closure lowering of ANY capturing function is
    // unsupported. Non-capturing closures (captures.empty()) never reach
    // this function, so they still work.
    (void)capture;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Entry Point
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* lowerClosure(AnonFuncExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    // ─── Interim: capturing closures not yet supported ──────────────────
    // The redesign represents a capture as (name, functionDepth), not a
    // ValueDeclAST*. Resolving that to an LLVM value and type requires the
    // CodeGen-side scope walker (per-function name→value and name→type
    // maps with an `enclosing` chain), which does not exist yet.
    //
    // Until it does, reject any closure with captures here, at the single
    // entry point, rather than silently producing a bogus environment.
    if (!expr->captures.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, expr->loc,
            "capturing closures are not yet supported by this codegen pass — "
            "the scope walker for capture type resolution is not implemented");
        return nullptr;
    }

    // ─── 1. Build the (empty) closure environment struct ────────────────
    // For a non-capturing closure this is just an empty struct; it exists
    // only to keep the fat-pointer shape uniform.
    llvm::StructType* envType = buildClosureEnvironment(expr, ctx);
    if (!envType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "failed to build closure environment");
        return nullptr;
    }

    // ─── 2. Create the closure function ─────────────────────────────────
    llvm::Function* closureFunc = createClosureFunction(expr, ctx);
    if (!closureFunc) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, expr->loc,
                                "failed to create closure function");
        return nullptr;
    }

    expr->closureFunction = closureFunc;
    expr->environmentType = envType;

    // ─── 3. Non-capturing: null env pointer ─────────────────────────────
    // No heap allocation, no retain, no release. The fat pointer's env
    // slot is null; emitClosureCall passes null through to the closure
    // function, which ignores it.
    llvm::Value* envPtr = llvm::ConstantPointerNull::get(
        llvm::PointerType::get(ctx.llvmCtx, 0));

    // ─── 4. Build the fat pointer { func, env } ─────────────────────────
    llvm::StructType* closureType = ctx.getClosureType();
    llvm::Value* closure = llvm::UndefValue::get(closureType);
    closure = ctx.builder.CreateInsertValue(
        closure,
        ctx.builder.CreatePointerCast(closureFunc,
            llvm::PointerType::get(ctx.llvmCtx, 0)),
        0);
    closure = ctx.builder.CreateInsertValue(closure, envPtr, 1);

    expr->llvmValue = closure;

    Trace::detail("Lowered non-capturing closure (0 captures)");
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

    // NOTE: there is no capture-load loop here. For a non-capturing closure
    // (the only kind that reaches this function — see the guard in
    // lowerClosure), the environment is empty and envPtr is null. Captured
    // values would be loaded from the env here once the scope walker exists.
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
        ctx.llvmCtx, "entry", closureFunc);
    ctx.builder.SetInsertPoint(entryBlock);

    // ─── 3. Lower parameters ────────────────────────────────────────────
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
            }
        }
    }

    // ─── 4. Lower the body ──────────────────────────────────────────────
    if (expr->body) {
        lowerStatement(expr->body, ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, expr->loc,
                                "anonymous function has no body");
        ctx.setCurrentFunction(nullptr);
        return false;
    }

    ctx.setCurrentFunction(nullptr);

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

    // ─── 2. Plain named function reference ─────────────────────────────────
    if (llvm::Function* fn = llvm::dyn_cast<llvm::Function>(callee)) {
        return ctx.builder.CreateCall(fn, args, name);
    }

    // ─── 3. Indirect function pointer - with runtime closure check ────────
    if (callee->getType()->isPointerTy()) {
        // ─── Check if this might be a closure at runtime ──────────────────
        // Use __lucid_is_closure to determine if the value is a closure.
        llvm::Function* isClosureFn = ctx.getRuntimeFn(RuntimeFn::IsClosure);
        llvm::Value* isClosure = ctx.builder.CreateCall(isClosureFn, {callee}, "is_closure");

        llvm::Function* func = ctx.getCurrentFunction();
        llvm::BasicBlock* closureBranch = llvm::BasicBlock::Create(
            ctx.llvmCtx, "call_closure", func);
        llvm::BasicBlock* plainBranch = llvm::BasicBlock::Create(
            ctx.llvmCtx, "call_plain", func);
        llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
            ctx.llvmCtx, "call_merge", func);

        ctx.builder.CreateCondBr(isClosure, closureBranch, plainBranch);

        // ─── Closure branch: load { func, env } and call ──────────────────
        ctx.builder.SetInsertPoint(closureBranch);
        // The callee pointer points to a closure struct { func, env }
        // Load the closure struct
        llvm::Type* closureType = ctx.getClosureType();
        llvm::Value* closureVal = ctx.builder.CreateLoad(closureType, callee, "closure_load");
        llvm::Value* funcPtr = ctx.builder.CreateExtractValue(closureVal, 0, "closure_func");
        llvm::Value* envPtr = ctx.builder.CreateExtractValue(closureVal, 1, "closure_env");
        llvm::Value* closureResult = emitClosureCall(funcPtr, envPtr, args, fnType->getReturnType(), ctx);
        ctx.builder.CreateBr(mergeBlock);

        // ─── Plain branch: cast and call directly ──────────────────────────
        ctx.builder.SetInsertPoint(plainBranch);
        llvm::Value* casted = ctx.builder.CreatePointerCast(
            callee, llvm::PointerType::get(fnType, 0), name + "_cast");
        llvm::Value* plainResult = ctx.builder.CreateCall(fnType, casted, args, name);
        ctx.builder.CreateBr(mergeBlock);

        // ─── Merge block: PHI the result ───────────────────────────────────
        ctx.builder.SetInsertPoint(mergeBlock);
        llvm::Type* resultType = fnType->getReturnType();
        if (resultType->isVoidTy()) {
            return nullptr;
        }
        llvm::PHINode* phi = ctx.builder.CreatePHI(resultType, 2, "call_result");
        phi->addIncoming(closureResult, closureBranch);
        phi->addIncoming(plainResult, plainBranch);
        return phi;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper Functions
// ─────────────────────────────────────────────────────────────────────────────

bool isClosureNeeded(const AnonFuncExprAST* expr) {
    if (!expr) return false;
    return expr->hasClosure || !expr->captures.empty();
}

} // namespace codegen