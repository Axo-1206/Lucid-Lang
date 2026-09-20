/// @file codegen/emit/EmitClosure.cpp
/// @brief Closure lowering — environment construction, capture binding,
///        body lowering, and env-drop glue.
///
/// ─── What a Closure Is ────────────────────────────────────────────────────
/// A closure is a fat pointer `{ fn, env }` where `env` points at a
/// refcounted heap allocation holding the captured variables. The
/// environment's layout is:
///
///     ┌──────────────────────────────────────┐
///     │  ClosureEnvHeader (16 bytes)         │
///     │    i64 refcount                      │
///     │    ptr drop                          │
///     ├──────────────────────────────────────┤
///     │  Captured values (the env struct)    │
///     │    field 0                           │
///     │    field 1                           │
///     │    ...                               │
///     └──────────────────────────────────────┘
///
/// The header is managed by the runtime (`__lucid_alloc_env`,
/// `__lucid_retain_env`, `__lucid_release_env`). The captures are managed
/// by the emitter. The env pointer stored in the fat pointer points at
/// the **header**, not at the captures; the runtime knows to skip 16
/// bytes when it passes the captures to the drop function.
///
/// ─── The Three Parts of Lowering ──────────────────────────────────────────
///
///   1. **Environment struct type** — an LLVM struct whose fields are the
///      captured variables, in capture order. Built by
///      `buildClosureEnvironment`.
///
///   2. **Closure function** — the LLVM function that implements the
///      closure body. Its signature is `R fn(ptr env, declaredParams...)`.
///      Built by `createClosureFunction` + `emitClosureBody`.
///
///   3. **Env-drop function** — a `void fn(ptr data)` that releases what
///      the captures own. Built by `buildEnvDropFunction`. Only generated
///      when the closure has by-value `cls` captures.
///
/// `emitAnonFunc` orchestrates the three: builds the env struct, creates
/// the closure function, allocates the env, stores the captures, and
/// constructs the fat pointer.
///
/// ─── Capture Handling ─────────────────────────────────────────────────────
/// A capture is either **by value** or **by reference** (Sema decides,
/// based on whether the closure might outlive the enclosing frame's
/// binding).
///
///   - **By value**: the env holds a copy of the value. The emitter
///     retains the env of a by-value `cls` capture (so the captured
///     closure's env outlives the outer frame). The closure body sees a
///     fresh alloca holding the copy; mutations don't reach the outer
///     frame.
///
///   - **By reference**: the env holds a pointer to the enclosing
///     frame's storage. The closure body binds the pointer directly; it
///     reads and writes the outer frame's slot. No retain — the outer
///     frame holds the claim, and its own scope-exit cleanup releases it.
///     Sema guarantees the closure doesn't outlive the outer frame.
///
/// ─── The Nested FunctionState ─────────────────────────────────────────────
/// The closure body is emitted inside a nested `FunctionState`, constructed
/// on the stack in `emitClosureBody`. Its constructor saves the enclosing
/// function's scalar state and the builder's insertion point; its
/// destructor restores them. The scope stack, loop stack, and value map
/// start empty for the closure body.
///
/// The `saveBinding` calls for captured declarations are what make the
/// closure body's rebinding of a captured name not clobber the enclosing
/// frame's binding for the same declaration. See the `saveBinding`
/// documentation in `FunctionState.hpp` for the mechanism.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <cassert>
#include <string>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitAnonFunc — the public closure-literal emitter
// ─────────────────────────────────────────────────────────────────────────────
//
// Orchestrates the three parts of closure lowering:
//
//   1. Build (or retrieve) the env struct type.
//   2. Create (or retrieve) the closure function (which emits the body).
//   3. If the closure has captures:
//        a. Build the env-drop function (or null if not needed).
//        b. Allocate the env via `__lucid_alloc_env`.
//        c. Store each capture into its env field.
//      Otherwise: the env pointer is null.
//   4. Construct the fat pointer.
//
// The result is `Owned` — the caller takes over the fat pointer's claim.
// The fat pointer's `env` field is the claim; dropping the fat pointer
// calls `__lucid_release_env` on that field.

Val Emitter::emitAnonFunc(AnonFuncExprAST* expr) {
    assert(expr && "emitAnonFunc() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── 1. Environment struct type ───────────────────────────────────────
    llvm::StructType* envTy = buildClosureEnvironment(expr);
    if (!envTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "failed to build the closure environment type");
        return {};
    }

    // ─── 2. Closure function ──────────────────────────────────────────────
    llvm::Function* closureFn = createClosureFunction(expr);
    if (!closureFn) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "failed to create the closure function");
        return {};
    }

    // ─── 3. Environment allocation and capture storage ────────────────────
    llvm::Value* envPtr = nullptr;

    if (expr->captures.empty()) {
        // Non-capturing closure: the env is null. The runtime handles
        // retain/release of a null env as a no-op.
        envPtr = llvm::ConstantPointerNull::get(
            llvm::PointerType::get(program.llvmContext(), 0));
    } else {
        // ─── 3a. Env-drop function ────────────────────────────────────────
        // Returns null if the env owns nothing that needs releasing.
        llvm::Function* dropFn = buildEnvDropFunction(expr, envTy);

        // ─── 3b. Env size ─────────────────────────────────────────────────
        // The runtime's `__lucid_alloc_env` takes the size of the DATA
        // portion (the captures), not the total. It adds the header
        // internally.
        const llvm::DataLayout& dl = program.module().getDataLayout();
        uint64_t envSize = dl.getTypeAllocSize(envTy).getFixedValue();
        if (envSize == 0) envSize = 1;  // degenerate empty env

        // ─── 3c. Call __lucid_alloc_env ───────────────────────────────────
        llvm::Value* sizeConst = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(program.llvmContext()), envSize);
        llvm::Value* dropFnPtr = dropFn
            ? static_cast<llvm::Value*>(dropFn)
            : static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(
                  llvm::PointerType::get(program.llvmContext(), 0)));

        llvm::Value* rawEnv = program.abi().AllocEnv(
            b, sizeConst, dropFnPtr);

        if (!rawEnv) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "failed to allocate the closure environment");
            return {};
        }

        envPtr = rawEnv;

        // ─── 3d. Store captures ───────────────────────────────────────────
        // For each capture:
        //   - Look up the captured value in the ENCLOSING function's
        //     value map. `func()` at this point is the enclosing
        //     `FunctionState` (the closure body hasn't been entered yet).
        //   - Load from the outer storage if the binding is a pointer
        //     (unless it's a `cls` FuncDecl, whose binding is the fat
        //     pointer by value).
        //   - For a by-value `cls` capture, retain the captured env.
        //   - For a by-reference capture, store the outer storage pointer
        //     (the binding itself), not the loaded value.
        //   - GEP into the env struct at `capture.index`.
        //   - Store.
        for (const CapturedVariable& capture : expr->captures) {
            if (!capture.resolvedDecl) continue;

            TypeAST* capturedTy = capture.resolvedDecl->type;
            if (!capturedTy) continue;

            llvm::Type* fieldTy = envTy->getElementType(
                static_cast<unsigned>(capture.index));
            if (!fieldTy) continue;

            // Look up the enclosing binding.
            llvm::Value* outerBinding = func().lookupValue(
                capture.resolvedDecl);
            if (!outerBinding) {
                program.diagnostics.errorAt(
                    DiagCode::Backend_CodegenError, expr->loc,
                    "captured variable '", program.pool.lookup(capture.name),
                    "' has no LLVM binding in the enclosing function");
                return {};
            }

            // Determine what value goes into the env slot.
            llvm::Value* envValue = nullptr;

            if (capture.byReference) {
                // By-reference: store a pointer to the outer storage.
                //
                // The outer binding is a pointer to the outer frame's
                // storage (an alloca, a param slot, another env-loaded
                // pointer). Storing the binding itself puts that pointer
                // into the env slot. The closure body binds it directly.
                //
                // If the outer binding is a non-pointer (a `cls`
                // FuncDecl's fat pointer held by value), a by-reference
                // capture isn't meaningful — the closure can't mutate a
                // by-value binding through a pointer. Sema should reject
                // this; the emitter falls through to the by-value path.
                if (outerBinding->getType()->isPointerTy()) {
                    envValue = outerBinding;
                } else {
                    // Fallback: treat as by-value.
                    envValue = outerBinding;
                }
            } else {
                // By-value: load the outer value (if the binding is a
                // pointer) or use it directly.
                llvm::Value* loaded = outerBinding;
                if (outerBinding->getType()->isPointerTy()
                    && !capture.resolvedDecl->isa<FuncDeclAST>()) {
                    llvm::Type* outerTy = program.types().get(capturedTy);
                    if (outerTy) {
                        loaded = b.CreateLoad(
                            outerTy, outerBinding, "capture.load");
                    }
                }

                // Retain a by-value `cls` capture's env. The env holds
                // a claim on the captured closure's environment.
                if (capturedTy->isa<FuncTypeAST>()) {
                    FuncTypeAST* fn = capturedTy->as<FuncTypeAST>();
                    if (fn->shape == FuncShape::Cls) {
                        llvm::Value* capturedEnv = b.CreateExtractValue(
                            loaded, 1, "capture.retain.env");
                        program.abi().RetainEnv(b, capturedEnv);
                    }
                }

                // Retain a by-value OwnedBuffer capture's buffer? The
                // current language doesn't allow string captures by value
                // (Sema rejects), so this is a no-op. A follow-up adds
                // the deep-copy for `OwnedBuffer` and the copy-glue call
                // for `Aggregate`.
                envValue = loaded;
            }

            if (!envValue) continue;

            // ─── Coerce the env value to the field type ───────────────────
            if (envValue->getType() != fieldTy) {
                llvm::Value* coerced = coerceValueToType(
                    envValue, fieldTy, b);
                if (coerced) envValue = coerced;
            }

            // ─── GEP to the env field and store ───────────────────────────
            llvm::Value* fieldPtr = b.CreateStructGEP(
                envTy, envPtr, static_cast<unsigned>(capture.index),
                "env.field." + program.pool.lookup(capture.name));
            b.CreateStore(envValue, fieldPtr);
        }
    }

    // ─── 4. Fat pointer ───────────────────────────────────────────────────
    llvm::StructType* closureTy = program.types().closureType();
    if (!closureTy) return {};

    llvm::Value* fat = llvm::UndefValue::get(closureTy);
    fat = b.CreateInsertValue(fat, closureFn, 0, "closure.fn");
    fat = b.CreateInsertValue(fat, envPtr, 1, "closure.env");

    Trace::detail("Lowered closure literal '",
                  program.pool.lookup(expr->funcType ? expr->funcType->name
                                                       : expr->loc.file),
                  "' with ", expr->captures.size(), " capture(s)");

    return Val{fat, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// buildClosureEnvironment — the env struct type
// ─────────────────────────────────────────────────────────────────────────────
//
// The env struct is `{ field_0, field_1, ... }` in capture order. Each
// field's LLVM type is the runtime shape of the captured AST type:
//   - `cls`-shaped `FuncTypeAST`: `lucid.Closure` (`{ ptr, ptr }`).
//   - `fn`-shaped `FuncTypeAST`: `ptr` (a bare function pointer).
//   - Anything else: `program.types().get(capturedTy)`.
//
// The struct type is cached on the AST node so repeated calls return the
// same `llvm::StructType*`. This matters because two fat pointers of the
// same closure literal must have structurally identical env types — the
// LLVM verifier accepts a store of one to a field typed as the other only
// if they're the same `llvm::StructType*`.

llvm::StructType* Emitter::buildClosureEnvironment(AnonFuncExprAST* expr) {
    if (!expr) return nullptr;

    // ─── Cache hit ────────────────────────────────────────────────────────
    if (expr->environmentType) return expr->environmentType;

    // ─── Field types ──────────────────────────────────────────────────────
    std::vector<llvm::Type*> fieldTypes;
    fieldTypes.reserve(expr->captures.size());

    for (const CapturedVariable& capture : expr->captures) {
        if (!capture.resolvedDecl) {
            // A capture without a resolved declaration is a Sema bug.
            // Emitting a placeholder field would produce IR the verifier
            // rejects; skip the capture entirely.
            continue;
        }

        TypeAST* capturedTy = capture.resolvedDecl->type;
        if (!capturedTy) continue;

        llvm::Type* fieldTy = nullptr;

        if (capturedTy->isa<FuncTypeAST>()) {
            // Function-typed captures use the runtime shape, not the
            // function's signature type.
            FuncTypeAST* fn = capturedTy->as<FuncTypeAST>();
            fieldTy = (fn->shape == FuncShape::Cls)
                ? static_cast<llvm::Type*>(program.types().closureType())
                : static_cast<llvm::Type*>(
                      llvm::PointerType::get(program.llvmContext(), 0));
        } else {
            fieldTy = program.types().get(capturedTy);
        }

        if (fieldTy) {
            fieldTypes.push_back(fieldTy);
        }
    }

    // ─── Create the struct type ───────────────────────────────────────────
    // The name includes a counter so two different closure literals don't
    // collide. The struct is opaque until `setBody`.
    std::string name = "closure.env";
    llvm::StructType* envTy = llvm::StructType::create(
        program.llvmContext(), name);
    if (!envTy) return nullptr;
    envTy->setBody(fieldTypes);

    // ─── Cache and return ─────────────────────────────────────────────────
    expr->environmentType = envTy;
    return envTy;
}

// ─────────────────────────────────────────────────────────────────────────────
// createClosureFunction — the LLVM function implementing the body
// ─────────────────────────────────────────────────────────────────────────────
//
// The function's signature is `R fn(ptr env, declaredParams...)`. The env
// is always the first parameter. Its linkage is internal — closures are
// reachable only through their fat pointers, not through symbols.
//
// The function is cached on the AST node. A closure literal is lowered
// once per program; the cache is defensive.
//
// The body is emitted by `emitClosureBody`, which constructs a nested
// `FunctionState`, binds captures, allocates parameters, and runs the
// body's statements.

llvm::Function* Emitter::createClosureFunction(AnonFuncExprAST* expr) {
    if (!expr) return nullptr;

    // ─── Cache hit ────────────────────────────────────────────────────────
    if (expr->closureFunction) return expr->closureFunction;

    // ─── Function type ────────────────────────────────────────────────────
    // The `isClosure=true` flag tells `Types::functionType` to prepend
    // the env parameter.
    llvm::FunctionType* fnTy = program.types().functionType(
        expr->funcType, /*isClosure=*/true);
    if (!fnTy) return nullptr;

    // ─── Function name ────────────────────────────────────────────────────
    // A unique name per closure. The counter is per-emitter; a static
    // counter on `ProgramState` would also work.
    static uint64_t closureCounter = 0;
    std::string fnName = "closure." + std::to_string(closureCounter++);

    // ─── Create the function ──────────────────────────────────────────────
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, fnName,
        program.module());
    fn->getArg(0)->setName("env");

    // ─── Emit the body ────────────────────────────────────────────────────
    emitClosureBody(expr, fn, fn->getArg(0));

    // ─── Cache and return ─────────────────────────────────────────────────
    expr->closureFunction = fn;
    return fn;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitClosureBody — the closure's body, with a nested FunctionState
// ─────────────────────────────────────────────────────────────────────────────
//
// Steps:
//   1. Construct a nested `FunctionState`. Saves the enclosing function's
//      scalar state and the builder's insertion point; installs the
//      closure as the current function.
//   2. Create the entry block, set the insertion point.
//   3. Push the closure's function-level scope.
//   4. Bind captures:
//        - `saveBinding(capture.resolvedDecl)` — save the enclosing
//          binding so it's restored when this body finishes.
//        - GEP to the env field, load the captured value.
//        - By-reference: bind the loaded pointer directly (it points at
//          the outer frame's storage).
//        - By-value: spill the loaded value to a fresh alloca, bind it,
//          mark alive.
//   5. Allocate parameters (same shape as `emitFuncBody`).
//   6. Emit the body: `emitBlock` or `emitReturnStmt`.
//   7. Fall-through cleanup for the function-level scope.
//   8. Fallback terminator.
//   9. FunctionState destructs — restores enclosing state.

void Emitter::emitClosureBody(AnonFuncExprAST* expr,
                              llvm::Function* closureFn,
                              llvm::Value* envPtr) {
    assert(expr && "emitClosureBody() with null expression");
    assert(closureFn && "emitClosureBody() with null closure function");

    // ─── 1. Nested FunctionState ──────────────────────────────────────────
    // Constructing a `FunctionState` saves the enclosing state and
    // installs the closure as the current function. The destructor
    // restores the enclosing state when this method returns.
    TypeAST* returnTy = expr->funcType ? expr->funcType->returnType : nullptr;
    FunctionState closureState(program, closureFn, returnTy);
    closureState.setEnvironmentPtr(envPtr);

    // ─── 2. Entry block ───────────────────────────────────────────────────
    llvm::IRBuilder<>& b = program.builder();
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(
        program.llvmContext(), "entry", closureFn);
    b.SetInsertPoint(entry);

    // ─── 3. Function-level scope ──────────────────────────────────────────
    func().pushScope();

    // ─── 4. Bind captures ─────────────────────────────────────────────────
    llvm::StructType* envTy = expr->environmentType;
    if (!envTy) {
        // If the env type wasn't set, the closure was created without
        // `buildClosureEnvironment` being called first — a bug in the
        // emitter's orchestration. Bail with a diagnostic rather than
        // producing malformed IR.
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, expr->loc,
            "closure has no environment type");
        return;
    }

    for (const CapturedVariable& capture : expr->captures) {
        if (!capture.resolvedDecl) continue;

        // ─── Save the enclosing binding ───────────────────────────────────
        // The closure body is about to install its own binding for the
        // captured declaration in the closure's value map. The enclosing
        // frame's binding for the same declaration must be restored when
        // the closure body finishes.
        func().saveBinding(capture.resolvedDecl);

        // ─── Load the captured value from the env ─────────────────────────
        llvm::Type* fieldTy = envTy->getElementType(
            static_cast<unsigned>(capture.index));
        if (!fieldTy) continue;

        llvm::Value* fieldPtr = b.CreateStructGEP(
            envTy, envPtr, static_cast<unsigned>(capture.index),
            "capture." + program.pool.lookup(capture.name));
        llvm::Value* captured = b.CreateLoad(
            fieldTy, fieldPtr,
            "capture.value." + program.pool.lookup(capture.name));

        if (capture.byReference) {
            // ─── By-reference: bind the loaded pointer directly ───────────
            // The env slot holds a pointer to the outer frame's storage.
            // The closure body's binding for the declaration is that
            // pointer. Loads and stores through it reach the outer frame.
            func().storeValue(capture.resolvedDecl, captured);
        } else {
            // ─── By-value: spill to a fresh alloca ────────────────────────
            // The env slot holds a copy. The closure body gets its own
            // storage slot so mutations don't reach the outer frame.
            llvm::AllocaInst* spill = createEntryAlloca(
                fieldTy,
                "capture.spill." + program.pool.lookup(capture.name));
            if (!spill) continue;

            b.CreateStore(captured, spill);
            func().storeValue(capture.resolvedDecl, spill);

            // ─── Mark alive ───────────────────────────────────────────────
            // A by-value resource capture is the closure body's
            // responsibility to release at scope exit. The env's own
            // drop function releases the ORIGINAL claim (the one the
            // outer frame held and the closure retained). The closure
            // body's copy is a separate claim.
            //
            // For scalars, `markAlive` is a no-op (their drop is a
            // no-op). For by-value `cls` captures, the closure retained
            // the env when it stored the capture; the closure body's
            // copy is the retained claim, and scope-exit cleanup
            // releases it. That's correct.
            if (classifyResource(capture.resolvedDecl) != ResourceKind::None) {
                func().markAlive(capture.resolvedDecl);
            }
        }
    }

    // ─── 5. Parameters ────────────────────────────────────────────────────
    // The closure function's signature is `R fn(ptr env, params...)`. The
    // env is arg 0; the declared parameters start at arg 1.
    if (expr->funcType) {
        size_t argIdx = 1;
        for (FuncTypeAST* stage = expr->funcType; stage;
             stage = stage->getNext()) {
            for (ParamAST* param : stage->params) {
                if (argIdx >= closureFn->arg_size()) break;

                llvm::Value* arg = closureFn->getArg(argIdx++);
                llvm::Type* paramTy = program.types().get(param->type);
                if (!paramTy) continue;

                llvm::AllocaInst* alloca = createEntryAlloca(
                    paramTy, program.pool.lookup(param->name));
                if (!alloca) continue;

                b.CreateStore(arg, alloca);
                func().storeValue(param, alloca);

                if (classifyResource(param) != ResourceKind::None) {
                    func().markAlive(param);
                }
            }
        }
    }

    // ─── 6. Body ──────────────────────────────────────────────────────────
    if (expr->body) {
        if (expr->body->isa<BlockStmtAST>()) {
            emitBlock(expr->body->as<BlockStmtAST>());
        } else if (expr->body->isa<ReturnStmtAST>()) {
            emitReturnStmt(expr->body->as<ReturnStmtAST>());
        }
    }

    // ─── 7. Fall-through cleanup ──────────────────────────────────────────
    // If the body ended in a terminator (an explicit `return`), its
    // unwind already cleared the scope's alive sets, and
    // `emitScopeFallthrough` is a no-op. If the body fell through, this
    // releases any live by-value captures and parameters.
    emitScopeFallthrough();
    func().popScope();

    // ─── 8. Fallback terminator ───────────────────────────────────────────
    llvm::BasicBlock* cur = b.GetInsertBlock();
    if (cur && !cur->getTerminator()) {
        llvm::Type* retTy = closureFn->getReturnType();
        if (retTy->isVoidTy()) {
            b.CreateRetVoid();
        } else {
            b.CreateRet(llvm::UndefValue::get(retTy));
        }
    }

    // ─── 9. FunctionState destructs (RAII) ────────────────────────────────
    // The destructor restores the enclosing function's scalar state and
    // the builder's insertion point. The `savedBindings` mechanism
    // restores any enclosing bindings the closure body clobbered.
}

// ─────────────────────────────────────────────────────────────────────────────
// buildEnvDropFunction — the env-drop glue
// ─────────────────────────────────────────────────────────────────────────────
//
// The env's drop function is called by `ClosureEnvHeader::release` when
// the refcount hits zero. It receives a pointer to the DATA portion of
// the env (i.e. the start of the captures struct), and releases whatever
// the captures own.
//
// ─── What Needs Releasing ─────────────────────────────────────────────────
// Today, the emitter generates a drop function that releases by-value
// `cls` captures. A by-value `cls` capture is a fat pointer whose env the
// closure retained when it stored the capture; releasing the env when the
// closure dies is the counterpart.
//
// The function is NOT generated when there are no by-value `cls` captures,
// because there's nothing to release. The runtime treats a null drop
// function as a no-op.
//
// ─── By-Value Strings and Aggregates ──────────────────────────────────────
// A by-value `string` capture owns a buffer; the env must free it when
// the env dies. A by-value aggregate capture likewise. The current
// language doesn't allow those captures by value (Sema rejects), so the
// emitter doesn't generate drop code for them. A follow-up adds the
// `Ownership::drop` call for each by-value resource capture.

llvm::Function* Emitter::buildEnvDropFunction(AnonFuncExprAST* expr,
                                              llvm::StructType* envTy) {
    if (!expr || !envTy) return nullptr;

    // ─── Does the env need a drop function? ──────────────────────────────
    bool needsDrop = false;
    for (const CapturedVariable& capture : expr->captures) {
        if (capture.byReference) continue;
        TypeAST* capturedTy = capture.resolvedDecl
            ? capture.resolvedDecl->type
            : nullptr;
        if (capturedTy && capturedTy->isa<FuncTypeAST>()) {
            FuncTypeAST* fn = capturedTy->as<FuncTypeAST>();
            if (fn->shape == FuncShape::Cls) {
                needsDrop = true;
                break;
            }
        }
    }
    if (!needsDrop) return nullptr;

    // ─── Create the function ──────────────────────────────────────────────
    // Signature: `void drop_N(ptr data)`.
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), {ptrTy}, /*isVarArg=*/false);

    static uint64_t dropCounter = 0;
    std::string name = "closure.env.drop." + std::to_string(dropCounter++);

    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, name, program.module());
    fn->getArg(0)->setName("data");

    // ─── Save the caller's insertion point ────────────────────────────────
    // The drop function is emitted at module scope while the enclosing
    // function's lowering is in progress. Saving and restoring the
    // insertion point keeps the enclosing function's emission intact.
    llvm::IRBuilderBase::InsertPointGuard guard(program.builder());
    llvm::IRBuilder<>& b = program.builder();

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    b.SetInsertPoint(entry);

    // ─── Release each by-value cls capture ────────────────────────────────
    llvm::Type* headerTy = llvm::StructType::get(
        ctx, {llvm::Type::getInt64Ty(ctx), ptrTy});
    // The env pointer is `data`; the captures struct starts at `data`.
    // The `data` pointer is to the captures, not the header, so the
    // GEP for field N is `gep envTy, data, 0, N`.
    llvm::Value* captures = b.CreatePointerCast(
        fn->getArg(0), ptrTy, "env.captures");

    for (const CapturedVariable& capture : expr->captures) {
        if (capture.byReference) continue;

        TypeAST* capturedTy = capture.resolvedDecl
            ? capture.resolvedDecl->type
            : nullptr;
        if (!capturedTy || !capturedTy->isa<FuncTypeAST>()) continue;
        FuncTypeAST* fnTy = capturedTy->as<FuncTypeAST>();
        if (fnTy->shape != FuncShape::Cls) continue;

        // GEP to the capture's field.
        llvm::Value* fieldPtr = b.CreateStructGEP(
            envTy, captures, static_cast<unsigned>(capture.index),
            "drop.field." + program.pool.lookup(capture.name));

        // Load the fat pointer.
        llvm::Type* fieldTy = envTy->getElementType(
            static_cast<unsigned>(capture.index));
        llvm::Value* fat = b.CreateLoad(
            fieldTy, fieldPtr, "drop.value");

        // Extract the env field and release it.
        llvm::Value* capturedEnv = b.CreateExtractValue(
            fat, 1, "drop.env");
        program.abi().ReleaseEnv(b, capturedEnv);
    }

    b.CreateRetVoid();
    return fn;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitClosureFuncDecl — a `cls`-shaped named function declaration
// ─────────────────────────────────────────────────────────────────────────────
//
// A `cls`-shaped `FuncDeclAST` binds a name to a runtime closure fat
// pointer. The lowering:
//
//   1. Emit the initializer. If it's an `AnonFuncExprAST`, the emitter's
//      `emitAnonFunc` produces the fat pointer. If it's any other
//      expression (a reference to another closure, a call returning a
//      closure), the emitter's `emit` handles it.
//   2. Store the fat pointer in the value map, keyed on the declaration.
//   3. Mark the binding alive so scope-exit cleanup releases its env.
//
// The binding is NOT stored through an alloca. A `cls` value is a fat
// pointer held by value; the value map is its storage. `emitIdentifier`'s
// `FuncDeclAST` branch reads it back directly, without a load.

void Emitter::emitClosureFuncDecl(FuncDeclAST* decl) {
    assert(decl && "emitClosureFuncDecl() with null declaration");

    // ─── Idempotency ──────────────────────────────────────────────────────
    // If the value map already holds a binding for this declaration, the
    // declaration has been lowered. Skip.
    if (func().lookupValue(decl)) return;

    // ─── Need an initializer ──────────────────────────────────────────────
    if (!decl->init) {
        program.diagnostics.errorAt(
            DiagCode::Sem_MissingFuncBody, decl->loc,
            "cls-shaped function '", program.pool.lookup(decl->name),
            "' has no body or initializer");
        return;
    }

    // ─── Emit the initializer ─────────────────────────────────────────────
    // If it's a block body, `emitAnonFunc` produces the fat pointer. If
    // it's a reference expression, `emit` produces it (via the
    // identifier or call path).
    Val closureVal;
    if (decl->init->isa<AnonFuncExprAST>()) {
        closureVal = emitAnonFunc(decl->init->as<AnonFuncExprAST>());
    } else {
        closureVal = emit(decl->init);
    }
    if (!closureVal.isValid()) return;

    // ─── Coerce to the declared function type ─────────────────────────────
    closureVal = coerceTo(closureVal, decl->funcType);
    if (!closureVal.isValid()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_TypeMismatch, decl->loc,
            "cls-shaped function '", program.pool.lookup(decl->name),
            "' initializer cannot be coerced to its declared type");
        return;
    }

    // ─── Register the binding ─────────────────────────────────────────────
    // The fat pointer is stored by value in the value map, not through
    // an alloca. `emitIdentifier`'s `FuncDeclAST` branch reads it back.
    func().storeValue(decl, closureVal.v);

    // ─── Mark alive ───────────────────────────────────────────────────────
    // The binding owns the fat pointer's env claim. Scope-exit cleanup
    // releases it.
    func().markAlive(decl);

    Trace::detail("Lowered cls-shaped function '",
                  program.pool.lookup(decl->name), "' as a closure value");
}

} // namespace codegen