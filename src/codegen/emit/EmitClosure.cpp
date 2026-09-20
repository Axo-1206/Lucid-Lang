/// @file codegen/emit/EmitClosure.cpp
/// @brief Closure lowering: environment construction, capture binding,
///        body lowering, and env-drop glue.
///
/// ─── What A Closure Is ────────────────────────────────────────────────────
/// A closure is a fat pointer `{ fn, env }` where `env` points at a
/// refcounted heap allocation holding the captured variables. The
/// environment is described by `ClosureEnvHeader` (in
/// `src/runtime/ClosureEnvironment.hpp`):
///
///     struct ClosureEnvHeader {
///         atomic<uint32_t> refcount;
///         uint32_t size;
///         void (*drop)(void* data);
///     };
///     // followed by the captured data
///
/// ─── The Three Parts Of Lowering ──────────────────────────────────────────
///   1. Environment struct: an LLVM struct type whose fields are the
///      captured variables, in order. Built by `buildClosureEnvironment`.
///
///   2. Closure function: the LLVM function that implements the closure
///      body. It takes `ptr env` as its first parameter, followed by the
///      declared parameters. Built by `createClosureFunction`.
///
///   3. Closure value: the fat pointer `{ closure_fn, env_ptr }`. Built
///      by `emitAnonFunc` after allocating the environment and storing
///      the captures.
///
/// ─── Capture Handling ─────────────────────────────────────────────────────
/// A capture is either by-value or by-reference (Sema decides). By-value
/// captures copy the value into the environment; if the captured value is
/// a `cls` closure, the copy retains its environment. By-reference
/// captures store a pointer to the captured binding's storage.
///
/// ─── The Env-Drop Function ────────────────────────────────────────────────
/// Every environment whose captures include a `cls` value must release
/// those captures' environments when it dies. This is emitted as a
/// separate LLVM function that the runtime's `ClosureEnvHeader::release`
/// calls when the refcount hits zero.
///
/// ─── Why A Nested FunctionState ───────────────────────────────────────────
/// The old code had a `ClosureBodyScope` RAII class. The new design uses
/// a nested `FunctionState`, which does the same thing but reuses the
/// general function-lowering machinery. This is what makes closure bodies
/// behave exactly like top-level function bodies for scope management.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitAnonFunc — the public closure literal emitter
// ─────────────────────────────────────────────────────────────────────────────

Val Emitter::emitAnonFunc(AnonFuncExprAST* expr) {
    if (!expr) return {};

    // Build the environment struct type.
    llvm::StructType* envTy = buildClosureEnvironment(expr);
    if (!envTy) return {};

    // Create the LLVM function that implements the closure body.
    llvm::Function* closureFn = createClosureFunction(expr);
    if (!closureFn) return {};

    // If there are captures, allocate the environment and store them.
    // If not, the env pointer is null.
    llvm::Value* envPtr = nullptr;
    llvm::IRBuilder<>& b = program.builder();

    if (!expr->captures.empty()) {
        // Build the env-drop function if any captures are by-value cls.
        llvm::Function* dropFn = buildEnvDropFunction(expr, envTy);

        // Allocate the environment via __lucid_alloc_env.
        uint64_t envSize = program.types().sizeOf(...);  // or DataLayout directly
        llvm::Function* allocEnvFn =
            program.abi().declareOrGet(RuntimeFn::AllocEnv);
        llvm::Value* dropPtr = dropFn
            ? static_cast<llvm::Value*>(dropFn)
            : llvm::ConstantPointerNull::get(
                  llvm::PointerType::get(program.llvmContext(), 0));
        envPtr = b.CreateCall(
            allocEnvFn,
            {llvm::ConstantInt::get(
                 llvm::Type::getInt64Ty(program.llvmContext()), envSize),
             dropPtr},
            "env_ptr");
        envPtr = b.CreatePointerCast(
            envPtr, llvm::PointerType::get(envTy, 0), "typed_env");

        // Store each capture into its environment field.
        for (const CapturedVariable& capture : expr->captures) {
            // Load the captured value from the enclosing function's
            // bindings or from the current function's value map.
            llvm::Value* binding = func().lookupValue(capture.resolvedDecl);
            TypeAST* capturedTy = capture.resolvedDecl
                ? capture.resolvedDecl->type
                : nullptr;
            if (!binding || !capturedTy) continue;

            llvm::Type* fieldTy = program.types().get(capturedTy);
            if (!fieldTy) continue;

            // Load the value if the binding is an alloca.
            llvm::Value* storedValue;
            if (binding->getType()->isPointerTy()
                && !capture.resolvedDecl->isa<FuncDeclAST>()) {
                storedValue = b.CreateLoad(fieldTy, binding, "capture_load");
            } else {
                storedValue = binding;
            }

            // If the capture owns a cls environment, retain it.
            if (!capture.byReference
                && capturedTy->isa<FuncTypeAST>()
                && capturedTy->as<FuncTypeAST>()->shape == FuncShape::Cls) {
                llvm::Value* envField = b.CreateExtractValue(
                    storedValue, 1, "capture_env");
                llvm::Function* retainFn =
                    program.abi().declareOrGet(RuntimeFn::RetainEnv);
                b.CreateCall(retainFn, {envField});
            }

            // Store into the environment field.
            llvm::Value* fieldPtr = b.CreateStructGEP(
                envTy, envPtr, capture.index,
                "env_field_" + program.pool.lookup(capture.name));
            b.CreateStore(storedValue, fieldPtr);
        }
    } else {
        envPtr = llvm::ConstantPointerNull::get(
            llvm::PointerType::get(program.llvmContext(), 0));
    }

    // Construct the fat pointer.
    llvm::StructType* closureTy = program.types().closureType();
    llvm::Value* fat = llvm::UndefValue::get(closureTy);
    fat = b.CreateInsertValue(
        fat,
        b.CreatePointerCast(
            closureFn,
            llvm::PointerType::get(program.llvmContext(), 0)),
        0);
    fat = b.CreateInsertValue(fat, envPtr, 1);

    return Val{fat, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// buildClosureEnvironment — the env struct type
// ─────────────────────────────────────────────────────────────────────────────

llvm::StructType* Emitter::buildClosureEnvironment(AnonFuncExprAST* expr) {
    // For each capture, get the LLVM type of the captured value. The
    // resulting struct is `{ field_0, field_1, ... }` in capture order.
    //
    // The closure AST caches its environment type after the first build
    // so repeated emitters (e.g. a nested closure inside a closure)
    // reuse the same struct type.
    if (expr->environmentType) return expr->environmentType;

    std::vector<llvm::Type*> fieldTypes;
    for (const CapturedVariable& capture : expr->captures) {
        TypeAST* capturedTy = capture.resolvedDecl
            ? capture.resolvedDecl->type
            : nullptr;
        if (!capturedTy) continue;

        llvm::Type* fieldTy = nullptr;
        if (capturedTy->isa<FuncTypeAST>()) {
            // Function-typed captures use the runtime shape (ptr for fn,
            // lucid.Closure for cls).
            FuncTypeAST* fn = capturedTy->as<FuncTypeAST>();
            fieldTy = fn->shape == FuncShape::Cls
                ? program.types().closureType()
                : llvm::PointerType::get(program.llvmContext(), 0);
        } else {
            fieldTy = program.types().get(capturedTy);
        }
        if (fieldTy) fieldTypes.push_back(fieldTy);
    }

    std::string envName = "closure_env_" + std::to_string(envCounter_++);
    llvm::StructType* envTy = llvm::StructType::create(
        program.llvmContext(), fieldTypes, envName);

    expr->environmentType = envTy;
    return envTy;
}

// ─────────────────────────────────────────────────────────────────────────────
// createClosureFunction — the LLVM function implementing the body
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* Emitter::createClosureFunction(AnonFuncExprAST* expr) {
    if (expr->closureFunction) return expr->closureFunction;

    // The signature is `R (ptr env, declared params...)`.
    llvm::FunctionType* fnTy = program.types().functionType(
        expr->funcType, /*isClosure=*/true);
    if (!fnTy) return nullptr;

    std::string fnName = "closure_" + std::to_string(closureCounter_++);
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, fnName, program.module());
    fn->getArg(0)->setName("env");

    // Name the declared parameters.
    size_t argIdx = 1;
    for (ParamAST* param : expr->funcType->params) {
        if (argIdx < fn->arg_size()) {
            fn->getArg(argIdx++)->setName(program.pool.lookup(param->name));
        }
    }

    // Emit the body.
    emitClosureBody(expr, fn, fn->getArg(0));

    expr->closureFunction = fn;
    return fn;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitClosureBody — the body, with a nested FunctionState
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitClosureBody(AnonFuncExprAST* expr,
                               llvm::Function* closureFn,
                               llvm::Value* envPtr) {
    // ─── Enter the closure's FunctionState ────────────────────────────────
    // Constructing a nested FunctionState saves the enclosing function's
    // state, installs the closure as the current function, and starts
    // with an empty scope stack.
    FunctionState closureState(
        program, closureFn, expr->funcType->returnType);
    closureState.setEnvironmentPtr(envPtr);

    // ─── Create the entry block ───────────────────────────────────────────
    llvm::IRBuilder<>& b = program.builder();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(program.llvmContext(), "entry", closureFn);
    b.SetInsertPoint(entry);

    // ─── Push the closure's function-level scope ──────────────────────────
    closureState.pushScope();

    // ─── Bind captures ────────────────────────────────────────────────────
    // For each capture:
    //   1. Save the enclosing function's binding for the declaration.
    //   2. Load the value from the environment.
    //   3. Store the new binding (alloca for by-value; loaded pointer
    //      for by-reference).
    llvm::StructType* envTy = expr->environmentType;
    for (const CapturedVariable& capture : expr->captures) {
        if (!capture.resolvedDecl) continue;

        // Save the enclosing binding so it can be restored when the
        // closure body finishes.
        closureState.saveBinding(capture.resolvedDecl);

        // Load the captured value from the env.
        llvm::Value* fieldPtr = b.CreateStructGEP(
            envTy, envPtr, capture.index,
            "captured_" + program.pool.lookup(capture.name));
        llvm::Type* fieldTy = envTy->getElementType(capture.index);
        llvm::Value* captured = b.CreateLoad(
            fieldTy, fieldPtr,
            "load_captured_" + program.pool.lookup(capture.name));

        if (capture.byReference) {
            // The env field holds a pointer to the captured binding's
            // storage. Bind directly.
            closureState.storeValue(capture.resolvedDecl, captured);
        } else {
            // The env field holds a copy. Spill to a fresh alloca so
            // mutations inside the closure don't reach the original.
            llvm::AllocaInst* spill = b.CreateAlloca(
                fieldTy, nullptr,
                "capture_spill_" + program.pool.lookup(capture.name));
            b.CreateStore(captured, spill);
            closureState.storeValue(capture.resolvedDecl, spill);
        }
    }

    // ─── Allocate parameters ──────────────────────────────────────────────
    size_t argIdx = 1;
    for (ParamAST* param : expr->funcType->params) {
        if (argIdx >= closureFn->arg_size()) break;
        llvm::Value* argVal = closureFn->getArg(argIdx++);

        llvm::Type* paramTy = program.types().get(param->type);
        if (!paramTy) continue;

        llvm::AllocaInst* alloca = b.CreateAlloca(
            paramTy, nullptr, program.pool.lookup(param->name));
        b.CreateStore(argVal, alloca);
        closureState.storeValue(param, alloca);

        if (ownsResource(param)) {
            closureState.markAlive(param);
        }
    }

    // ─── Emit the body ────────────────────────────────────────────────────
    if (expr->body) {
        if (expr->body->isa<BlockStmtAST>()) {
            emitBlock(expr->body->as<BlockStmtAST>());
        } else if (expr->body->isa<ReturnStmtAST>()) {
            emitReturnStmt(expr->body->as<ReturnStmtAST>());
        }
    }

    // ─── Pop the closure's scope ──────────────────────────────────────────
    closureState.popScope();

    // ─── Fall-through terminator ──────────────────────────────────────────
    llvm::BasicBlock* cur = b.GetInsertBlock();
    if (cur && !cur->getTerminator()) {
        llvm::Type* retTy = closureFn->getReturnType();
        if (retTy->isVoidTy()) {
            b.CreateRetVoid();
        } else {
            b.CreateRet(llvm::UndefValue::get(retTy));
        }
    }

    // ClosureState's destructor restores the enclosing function's state
    // (including the saved bindings) when it goes out of scope here.
}

// ─────────────────────────────────────────────────────────────────────────────
// buildEnvDropFunction — the env-drop glue
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* Emitter::buildEnvDropFunction(AnonFuncExprAST* expr,
                                               llvm::StructType* envTy) {
    // Scan captures for by-value cls values. If none, the environment
    // owns nothing that needs releasing — return null.
    bool needsDrop = false;
    for (const CapturedVariable& capture : expr->captures) {
        if (capture.byReference) continue;
        TypeAST* capturedTy = capture.resolvedDecl
            ? capture.resolvedDecl->type
            : nullptr;
        if (capturedTy && capturedTy->isa<FuncTypeAST>()
            && capturedTy->as<FuncTypeAST>()->shape == FuncShape::Cls) {
            needsDrop = true;
            break;
        }
    }
    if (!needsDrop) return nullptr;

    // Generate: `void closure_env_drop_N(ptr data)`.
    // `data` points at the captured data portion (past the env header).
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), {ptrTy}, false);

    std::string name = "closure_env_drop_"
                     + std::to_string(envDropCounter_++);
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, name, program.module());
    fn->getArg(0)->setName("data");

    // Save the caller's insertion point — glue generation happens
    // during another function's lowering.
    llvm::IRBuilderBase::InsertPointGuard guard(program.builder());
    llvm::IRBuilder<>& b = program.builder();

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(ctx, "entry", fn);
    b.SetInsertPoint(entry);

    // For each by-value cls capture: extract the env pointer from the
    // env field and release it.
    llvm::Function* releaseFn =
        program.abi().declareOrGet(RuntimeFn::ReleaseEnv);

    for (const CapturedVariable& capture : expr->captures) {
        if (capture.byReference) continue;
        TypeAST* capturedTy = capture.resolvedDecl
            ? capture.resolvedDecl->type
            : nullptr;
        if (!capturedTy || !capturedTy->isa<FuncTypeAST>()
            || capturedTy->as<FuncTypeAST>()->shape != FuncShape::Cls) {
            continue;
        }

        llvm::Value* fieldPtr = b.CreateStructGEP(
            envTy, fn->getArg(0), capture.index,
            "drop_field_" + program.pool.lookup(capture.name));
        llvm::Value* fat = b.CreateLoad(
            envTy->getElementType(capture.index), fieldPtr,
            "drop_cls_" + program.pool.lookup(capture.name));
        llvm::Value* envField =
            b.CreateExtractValue(fat, 1, "drop_env");
        b.CreateCall(releaseFn, {envField});
    }

    b.CreateRetVoid();
    return fn;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitClosureFuncDecl — the fat pointer for a cls-shaped named function
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitClosureFuncDecl(FuncDeclAST* decl) {
    // A cls-shaped FuncDeclAST is a value, not an LLVM function. The
    // value is a fat pointer { closure_fn, env }, constructed exactly
    // like an anonymous closure literal.
    //
    // The body lives on the `init` AnonFuncExprAST. If `init` is an anon,
    // emit it via emitAnonFunc and store the resulting fat pointer in the
    // value map under the declaration.
    //
    // A reference-body cls function (`init` is not an anon) aliases
    // another closure; emit the reference and store its result.
    if (!decl->init) return;

    Val value;
    if (decl->init->isa<AnonFuncExprAST>()) {
        value = emitAnonFunc(decl->init->as<AnonFuncExprAST>());
    } else {
        value = emit(decl->init);
    }
    if (!value.isValid()) return;

    func().storeValue(decl, value.v);
}

} // namespace codegen