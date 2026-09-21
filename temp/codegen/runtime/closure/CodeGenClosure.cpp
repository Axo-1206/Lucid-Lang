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
#include "codegen/CodeGen.hpp"
#include "codegen/memory/CodeGenAlloca.hpp"
#include "codegen/memory/CodeGenOwnership.hpp"
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
// Capture Field Type
// ─────────────────────────────────────────────────────────────────────────────

llvm::Type* getCaptureFieldType(CodeGenContext& ctx, const CapturedVariable& capture) {
    if (!capture.resolvedDecl || !capture.resolvedDecl->type) return nullptr;
    return getType(ctx, capture.resolvedDecl->type);
}

// ─────────────────────────────────────────────────────────────────────────────
// Env Ownership + Body Isolation Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// @brief True iff this capture makes the environment OWN a claim on a
///        closure environment.
///
/// By-value capture of a `cls` value copies the fat pointer and retains its
/// env (see lowerClosure). That retained claim lives inside the capturing
/// environment, so it must be released when the capturing environment dies.
/// This one predicate drives BOTH the retain in lowerClosure and the drop
/// function below, so the two can never drift apart.
static bool capturesOwnedClosureEnv(const CapturedVariable& capture) {
    if (capture.byReference) return false;
    if (!capture.resolvedDecl || !capture.resolvedDecl->type) return false;
    TypeAST* type = capture.resolvedDecl->type;
    return type->isa<FuncTypeAST>()
        && type->as<FuncTypeAST>()->shape == FuncShape::Cls;
}

static std::atomic<size_t> g_envDropCounter{0};

/// @brief Emit `void closure_env_drop_N(ptr data)` for this closure, or return
///        nullptr if its environment owns nothing that needs releasing.
///
/// The runtime calls it with a pointer to the environment's DATA portion when
/// the last reference goes away (see ClosureEnvHeader::release). It releases
/// the env of every by-value `cls` capture — the claims taken by the retains
/// in lowerClosure. Without it those retains are never balanced and every
/// captured closure env leaks.
///
/// __lucid_release_env is null-safe, so a captured non-capturing closure
/// (null env) needs no check here.
static llvm::Function* buildEnvDropFunction(AnonFuncExprAST* expr,
                                            llvm::StructType* envType,
                                            CodeGenContext& ctx) {
    bool needed = false;
    for (const CapturedVariable& capture : expr->captures) {
        if (capturesOwnedClosureEnv(capture)) { needed = true; break; }
    }
    if (!needed) return nullptr;

    // Emitting a new function must not disturb the caller's insertion point.
    llvm::IRBuilderBase::InsertPointGuard guard(ctx.builder);

    llvm::Type* ptrTy = llvm::PointerType::get(ctx.llvmCtx, 0);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx.llvmCtx), {ptrTy}, false);
    llvm::Function* dropFn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        "closure_env_drop_" + std::to_string(++g_envDropCounter), ctx.module);
    dropFn->getArg(0)->setName("data");

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx.llvmCtx, "entry", dropFn);
    ctx.builder.SetInsertPoint(entry);

    llvm::Function* releaseFn = ctx.getRuntimeFn(RuntimeFn::ReleaseEnv);
    for (const CapturedVariable& capture : expr->captures) {
        if (!capturesOwnedClosureEnv(capture)) continue;

        std::string name = ctx.pool.lookup(capture.name);
        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
            envType, dropFn->getArg(0), capture.index, "drop_field_" + name);
        llvm::Value* fat = ctx.builder.CreateLoad(
            envType->getElementType(capture.index), fieldPtr, "drop_cls_" + name);
        llvm::Value* env = ctx.builder.CreateExtractValue(fat, 1, "drop_env_" + name);
        ctx.builder.CreateCall(releaseFn, {env});
    }
    ctx.builder.CreateRetVoid();
    return dropFn;
}

namespace {

/// @brief RAII isolation for lowering ONE closure body.
///
/// A closure body is a different llvm::Function from the code that contains
/// the closure expression, but CodeGenContext keeps its per-function state in
/// single shared members. Lowering the body without isolating that state:
///   - leaves the IRBuilder inside the closure function, so the env
///     allocation and capture stores that lowerClosure emits next land in the
///     wrong function;
///   - lets `return` inside the closure (emitUnwindTo(0)) emit cleanup for the
///     ENCLOSING function's live variables, whose allocas belong to another
///     function (invalid IR), and puts the closure's own params in the
///     enclosing function's tracker so they are never released;
///   - lets the capture bindings written into ctx.values overwrite the
///     enclosing function's bindings for the same declarations.
///
/// The constructor takes everything out of the context; the destructor puts it
/// back on every exit path (including early returns and errors).
struct ClosureBodyScope {
    CodeGenContext& ctx;
    llvm::IRBuilderBase::InsertPointGuard insertGuard;   // restores block + point
    llvm::Function* prevFunc;
    llvm::Value* prevEnv;
    TypeAST* prevReturnType;
    decltype(CodeGenContext::liveTrackers) prevTrackers;
    decltype(CodeGenContext::loops) prevLoops;
    decltype(CodeGenContext::nullCoalesceStack) prevNullCoalesce;
    std::vector<std::pair<ValueDeclAST*, llvm::Value*>> savedBindings;

    explicit ClosureBodyScope(CodeGenContext& c)
        : ctx(c),
          insertGuard(c.builder),
          prevFunc(c.currentFunction),
          prevEnv(c.currentEnvPtr),
          prevReturnType(c.currentDeclaredReturnType),
          prevTrackers(std::move(c.liveTrackers)),
          prevLoops(std::move(c.loops)),
          prevNullCoalesce(std::move(c.nullCoalesceStack)) {
        // Moved-from vectors are valid but unspecified; make them empty.
        c.liveTrackers.clear();
        c.loops.clear();
        c.nullCoalesceStack.clear();
    }

    /// Remember the enclosing binding of `decl` before the closure body
    /// rebinds it to its own env-loaded value / spill slot.
    void saveBinding(ValueDeclAST* decl) {
        savedBindings.emplace_back(decl, ctx.lookupValue(decl));
    }

    ~ClosureBodyScope() {
        // Reverse order: if a decl was saved twice, the earliest (= the
        // enclosing function's) binding is applied last and wins.
        for (auto it = savedBindings.rbegin(); it != savedBindings.rend(); ++it) {
            if (it->second) ctx.values[it->first] = it->second;
            else            ctx.values.erase(it->first);
        }
        ctx.currentFunction = prevFunc;
        ctx.currentEnvPtr = prevEnv;
        ctx.currentDeclaredReturnType = prevReturnType;
        ctx.liveTrackers = std::move(prevTrackers);
        ctx.loops = std::move(prevLoops);
        ctx.nullCoalesceStack = std::move(prevNullCoalesce);
        // insertGuard's destructor runs after this body and restores the
        // builder's insertion point.
    }
};

} // anonymous namespace

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
        const llvm::DataLayout& dl = ctx.module->getDataLayout();
        uint64_t envSize = dl.getTypeAllocSize(envType);
        llvm::Value* envSizeVal = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx.llvmCtx), envSize);
        // Drop glue releases the envs of by-value cls captures when this
        // environment dies. Null when the env owns nothing to release.
        llvm::Function* dropFn = buildEnvDropFunction(expr, envType, ctx);
        llvm::Value* dropVal = dropFn
            ? static_cast<llvm::Value*>(dropFn)
            : llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx.llvmCtx, 0));
        envPtr = ctx.builder.CreateCall(allocEnv, {envSizeVal, dropVal}, "env_ptr");
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
                // A variable's binding is the ADDRESS of its storage: an
                // alloca, or (inside another closure) the pointer loaded from
                // that closure's env for a by-reference capture. Module-level
                // bindings never reach here — they live in the module
                // instance, not in ctx.values. A cls-shaped FuncDeclAST is
                // the exception: its binding is the fat-pointer VALUE.
                if (binding->getType()->isPointerTy()
                    && !capture.resolvedDecl->isa<FuncDeclAST>()) {
                    storedValue = ctx.builder.CreateLoad(
                        fieldType, binding,
                        "capture_" + ctx.pool.lookup(capture.name));
                } else {
                    storedValue = binding;
                }
            }

            if (capturesOwnedClosureEnv(capture)) {
                // A cls-typed capture is always a fat pointer. Retain its
                // environment; the matching release is the drop function
                // (buildEnvDropFunction). fn-typed captures are bare pointers
                // and have no environment ownership to retain.
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

    // ─── 1. Isolate per-function state ───────────────────────────────────
    // Saves the builder insertion point, current function / return type,
    // live trackers, loop stack, null-coalesce stack, and (via saveBinding)
    // the enclosing bindings of captured declarations. Everything is restored
    // when `scope` is destroyed, on every exit path.
    ClosureBodyScope scope(ctx);

    ctx.setCurrentFunction(closureFunc);
    ctx.currentDeclaredReturnType = funcType->returnType;

    // ─── 2. Create entry block + the closure's own function-level scope ──
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx, "entry", closureFunc);
    ctx.builder.SetInsertPoint(entryBlock);

    // Owns the closure's params. Popped below, BEFORE the fallback
    // terminator, so fall-through paths release them too.
    ctx.pushLiveScope();

    // ─── 3. Load captured values from the environment ───────────────────
    // This runs FIRST, before parameters and body, because the body's
    // identifier expressions resolve against `ctx.values` — the same map
    // these loads populate.
    //
    // By-reference captures: the env field holds a pointer to the
    //   captured binding's storage. Store that pointer directly under
    //   `resolvedDecl`; identifier loads through it see the live value.
    // By-value captures: the env field holds a copy. Spill it into a
    //   fresh alloca so mutations inside the closure do not reach the
    //   original, and store the alloca under `resolvedDecl`.
    //
    // The closure BORROWS these values: the env owns the claim (see
    // buildEnvDropFunction), so nothing captured is marked alive here.
    //
    // `resolvedDecl` is the ENCLOSING function's declaration, so its entry in
    // ctx.values is rebound here; saveBinding lets `scope` put it back for
    // the capture stores lowerClosure emits after this returns.
    for (const CapturedVariable& capture : expr->captures) {
        if (!capture.name.isValid() || !capture.resolvedDecl) continue;

        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
            envType, envPtr, capture.index,
            "captured_" + ctx.pool.lookup(capture.name));
        llvm::Type* fieldType = envType->getElementType(capture.index);
        llvm::Value* capturedValue = ctx.builder.CreateLoad(
            fieldType, fieldPtr,
            "load_captured_" + ctx.pool.lookup(capture.name));

        scope.saveBinding(capture.resolvedDecl);
        if (capture.byReference) {
            ctx.storeValue(capture.resolvedDecl, capturedValue);
        } else {
            llvm::AllocaInst* spill = ctx.builder.CreateAlloca(
                fieldType, nullptr,
                "capture_spill_" + ctx.pool.lookup(capture.name));
            ctx.builder.CreateStore(capturedValue, spill);
            ctx.storeValue(capture.resolvedDecl, spill);
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
    if (!expr->body) {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, expr->loc,
                                "anonymous function has no body");
        return false;
    }
    lowerStatement(expr->body, ctx);

    // ─── 6. Pop the function scope, THEN ensure a terminator ────────────
    // Order matters: popLiveScope skips its cleanup when the block already
    // ends in a terminator (an explicit `return` already unwound), so the
    // fallback terminator must be added after it, or fall-through paths would
    // never release the closure's owning params.
    ctx.popLiveScope();

    llvm::BasicBlock* endBlock = ctx.builder.GetInsertBlock();
    if (endBlock && !endBlock->getTerminator()) {
        llvm::Type* retType = closureFunc->getReturnType();
        if (retType->isVoidTy()) {
            ctx.builder.CreateRetVoid();
        } else {
            // Sema reports missing returns; keep the IR well-formed.
            ctx.builder.CreateRet(llvm::UndefValue::get(retType));
        }
    }

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


} // namespace codegen