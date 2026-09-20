/// @file codegen/emit/EmitDecl.cpp
/// @brief Declaration lowering — the `Emitter::emit(DeclAST*)` entry point
///        and its per-kind dispatch.
///
/// ─── Two Phases ───────────────────────────────────────────────────────────
/// Declarations are emitted in two phases by the pass runner:
///
///   1. Declare pass (DeclarePass.cpp) — every function gets a prototype;
///      every struct/enum gets its LLVM type. No bodies. This is what makes
///      forward references resolvable.
///
///   2. Define pass (DefinePass.cpp) — every function body is lowered.
///      Local variables are allocated and initialized.
///
/// `emit(DeclAST*)` is called in both phases, and both phases may route
/// through this file. For a `fn`-shaped function declaration, `emitFuncDecl`
/// is idempotent: if the prototype doesn't exist, it creates one; if the
/// body doesn't exist, it lowers one. The declare pass calls it with the
/// prototype absent and the body absent, so only the prototype is created.
/// The define pass calls it again with the prototype present and the body
/// absent, so only the body is created.
///
/// ─── Why The Two-Phase Design Works ───────────────────────────────────────
/// The critical property is: a function's body can call any other function
/// in the program, no matter where it appears in source order. That works
/// because by the time any body is lowered, every prototype exists.
///
/// The second critical property is: a struct can contain a field whose type
/// is another struct declared later. That works because `Types::structType`
/// creates an opaque struct on first reference, caches it, and fills in the
/// body when the declaration is lowered. A pointer to the opaque struct is
/// what the field holds, so the layout is stable.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emit — the public dispatch
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emit(DeclAST* decl) {
    if (!decl) return;

    // Skip anything Sema flagged as error-recovered.
    if (decl->hasSyntaxError) return;

    switch (decl->kind) {
        case ASTKind::FuncDecl:   return emitFuncDecl(decl->as<FuncDeclAST>());
        case ASTKind::VarDecl:    return emitVarDecl(decl->as<VarDeclAST>());
        case ASTKind::StructDecl: return emitStructDecl(decl->as<StructDeclAST>());
        case ASTKind::EnumDecl:   return emitEnumDecl(decl->as<EnumDeclAST>());

        case ASTKind::TraitDecl:
            // Traits are a compile-time-only construct. They have no LLVM
            // representation of their own; Sema checks their field contract
            // against implementing structs and then discards them.
            return;

        case ASTKind::ImportDecl:
            // Imports are resolved by the module loader before codegen runs.
            // There is nothing to emit.
            return;

        default:
            // Sema should have rejected any other declaration kind. Reaching
            // here is a compiler bug; the safest response is to emit nothing
            // rather than crash.
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitVarDecl — local variable declaration
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Module-Level vs. Local ───────────────────────────────────────────────
// A module-level variable's storage is a field in the module instance
// struct. The instance global is emitted by ModulePass, and the
// initialization is emitted by ModulePass::emitModuleInit. `emitVarDecl`
// is a no-op for module-level bindings — the emitter is not the layer that
// owns module state.
//
// A local variable gets an alloca. The initializer runs here. The binding
// is registered in the current `FunctionState` so subsequent identifiers
// resolve to it.
//
// ─── The `init != nullptr` Invariant ──────────────────────────────────────
// Sema guarantees:
//   - `const` bindings always have an initializer.
//   - Resource-owning bindings always have an initializer (or a default
//     synthesized by Sema for tagged types with `nil`/`err` defaults).
//   - A binding without an initializer is a scalar `let`.
//
// When `init` is null, the alloca is left undefined. Any load before the
// first store is a Sema-rejected program, so the emitter doesn't guard it.

void Emitter::emitVarDecl(VarDeclAST* decl) {
    assert(decl && "emitVarDecl() with null declaration");

    // Module-level bindings are handled by ModulePass. Skip.
    if (decl->isModuleLevel()) return;

    // ─── Storage type ─────────────────────────────────────────────────────
    llvm::Type* varTy = program.types().get(decl->type);
    if (!varTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, decl->loc,
            "variable '", program.pool.lookup(decl->name),
            "' has an unresolvable type");
        return;
    }

    // ─── Allocate the binding's storage ───────────────────────────────────
    // Entry-block allocation, not current-block. A `let` inside a loop
    // body would otherwise allocate a fresh slot per iteration, and the
    // slots would accumulate until the function returns.
    llvm::AllocaInst* alloca = createEntryAlloca(
        varTy, program.pool.lookup(decl->name));
    if (!alloca) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, decl->loc,
            "could not allocate storage for '", program.pool.lookup(decl->name),
            "' — no active function");
        return;
    }

    // ─── Register the binding ─────────────────────────────────────────────
    // Registering before emitting the initializer is what makes
    // `let x = x` (self-reference in an initializer) resolve to the
    // binding. Sema rejects such self-references, but registering first
    // means a miscompiled program will see `undef` rather than a null
    // pointer dereference.
    //
    // The binding is NOT marked alive yet. The store that writes the
    // initializer will do that. Marking it alive before the store would
    // make `store` try to drop the undefined old value.
    func().storeValue(decl, alloca);

    // ─── No initializer: done ─────────────────────────────────────────────
    if (!decl->init) {
        // A scalar `let` without an initializer. The alloca holds
        // undefined data; any load before the first store is a
        // Sema-rejected program, so we don't seed it with a zero.
        //
        // A resource-owning or `const` binding without an initializer is
        // a Sema bug. If we reached here, emitting an uninitialized slot
        // is safer than crashing; the diagnostic will surface on the
        // first load.
        return;
    }

    // ─── Emit and coerce the initializer ──────────────────────────────────
    Val initVal = emit(decl->init);
    if (!initVal.isValid()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, decl->init->loc,
            "failed to lower initializer for '",
            program.pool.lookup(decl->name), "'");
        return;
    }

    // Coerce to the declared type. This handles `fn → cls` widening,
    // integer width adjustment, and pointer casts.
    initVal = coerceTo(initVal, decl->type);
    if (!initVal.isValid()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_TypeMismatch, decl->loc,
            "initializer for '", program.pool.lookup(decl->name),
            "' cannot be coerced to its declared type");
        return;
    }

    // ─── Store through the single write path ──────────────────────────────
    // `store`:
    //   1. Calls `intoOwned` on the initializer — a Borrowed value (an
    //      identifier load, a field load) is deep-copied or retained; an
    //      Owned value transfers unchanged.
    //   2. Loads and drops the old value if `decl` is alive — it isn't,
    //      so this step is skipped.
    //   3. Stores the owned value.
    //   4. Marks `decl` alive, so scope-exit cleanup will release it.
    //
    // After `store` returns, the binding holds the claim, and the
    // binding's lifetime is managed by the scope stack.
    Place place{alloca, decl->type};
    store(place, initVal, decl);

    Trace::detail("Lowered local var '", program.pool.lookup(decl->name), "'");
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFuncDecl — function prototype or body, depending on phase
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── What This Function Does ──────────────────────────────────────────────
// It's called from both passes. Its job is to make the function "exist":
// after the declare pass, a prototype exists; after the define pass, a
// body exists. It's idempotent — calling it twice on the same declaration
// is a no-op the second time.
//
// ─── The Three Cases ──────────────────────────────────────────────────────
//
//   1. Foreign function — an external symbol. Emit a `declare` with
//      external linkage and no body. Idempotent.
//
//   2. `cls`-shaped function — a runtime fat pointer, not an LLVM function.
//      Emit the fat pointer and register it in the current value map. Only
//      meaningful inside a function body; at module level, `cls`-shaped
//      declarations are handled as module-level bindings (stored as a
//      field in the module instance).
//
//   3. `fn`-shaped function — a normal LLVM function. In the declare pass,
//      create the prototype. In the define pass, emit the body if the
//      function is still empty.

void Emitter::emitFuncDecl(FuncDeclAST* decl) {
    assert(decl && "emitFuncDecl() with null declaration");

    // ─── Generic templates ────────────────────────────────────────────────
    // A generic function is a family, not a value. Sema produced a
    // specialization for every concrete instantiation, and those
    // specializations are lowered as their own declarations (from
    // `module->specializations`). The template itself has no LLVM object.
    if (decl->isGeneric()) return;

    // ─── Foreign function: a `declare` ────────────────────────────────────
    if (decl->isForeignFunction) {
        emitForeignFuncDecl(decl);
        return;
    }

    // ─── `cls`-shaped function: a fat pointer value ──────────────────────
    // A `cls`-shaped declaration binds a name to a runtime fat pointer,
    // not to an LLVM function. At module level, the binding lives in the
    // module instance struct and is initialized by the module pass. Inside
    // a function body (a local `cls` declaration), the fat pointer is
    // constructed here and registered in the value map.
    FuncShape shape = decl->funcType ? decl->funcType->shape : FuncShape::Fn;
    if (shape == FuncShape::Cls) {
        // Module-level: skip. ModulePass handles the initializer.
        if (decl->isModuleLevel()) return;
        emitClosureFuncDecl(decl);
        return;
    }

    // ─── `fn`-shaped function: prototype and body ────────────────────────
    llvm::Function* fn = program.lookupFunction(decl);

    if (!fn) {
        // Declare pass: create the prototype.
        //
        // A function with no mangled name and no source name is a Sema
        // bug. Bail rather than creating an unnamed function.
        std::string name = decl->mangledName.isValid()
            ? program.pool.lookup(decl->mangledName)
            : program.pool.lookup(decl->name);
        if (name.empty()) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, decl->loc,
                "function declaration has no symbol name");
            return;
        }

        // Build the LLVM signature. `isClosure=false` because this is a
        // bare function; the env parameter only appears on the closure
        // implementation, which has a different decl (the anon's
        // FuncTypeAST, not the FuncDeclAST's).
        llvm::FunctionType* fnTy = program.types().functionType(
            decl->funcType, /*isClosure=*/false);
        if (!fnTy) {
            program.diagnostics.errorAt(
                DiagCode::Backend_InvalidIR, decl->loc,
                "function '", program.pool.lookup(decl->name),
                "' has an invalid signature");
            return;
        }

        // Linkage: an exported function is visible to other modules and
        // to the host; a private function is internal.
        llvm::GlobalValue::LinkageTypes linkage =
            decl->isExported ? llvm::GlobalValue::ExternalLinkage
                             : llvm::GlobalValue::InternalLinkage;

        fn = llvm::Function::Create(fnTy, linkage, name, program.module());

        // Name parameters for readable IR dumps. Parameter names come from
        // the declared signature (`decl->funcType`); the runtime signature
        // is on the anon's `funcType`, but the two have the same parameter
        // names by construction.
        size_t argIdx = 0;
        for (FuncTypeAST* stage = decl->funcType; stage; stage = stage->getNext()) {
            for (ParamAST* param : stage->params) {
                if (argIdx >= fn->arg_size()) break;
                if (!param->name.isValid()) {
                    ++argIdx;
                    continue;
                }
                fn->getArg(argIdx++)->setName(program.pool.lookup(param->name));
            }
        }

        program.storeFunction(decl, fn);

        Trace::detail("Declared function '", name, "'");
    }

    // ─── Define pass: emit the body if not emitted ────────────────────────
    // `fn->empty()` is true for a fresh prototype. After the body is
    // emitted, the function has at least one basic block, so a second
    // call to `emitFuncDecl` on the same declaration is a no-op.
    if (fn->empty()) {
        emitFuncBody(decl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitForeignFuncDecl — a bare `declare`, no body
// ─────────────────────────────────────────────────────────────────────────────
//
// A foreign function is an external symbol that generated code calls into.
// The signature comes from `decl->funcType`; the linkage is external; no
// body is emitted. The `@[foreign]` attribute (or equivalent) on the
// declaration is what Sema keyed on to set `isForeignFunction`.
//
// The `mangledName` of a foreign function is usually just its source name
// (Sema leaves it bare for `@[foreign]`), but the emitter falls back to
// the source name if Sema didn't set one.

void Emitter::emitForeignFuncDecl(FuncDeclAST* decl) {
    assert(decl && "emitForeignFuncDecl() with null declaration");
    assert(decl->isForeignFunction
           && "emitForeignFuncDecl() called on a non-foreign function");

    // ─── Idempotency ──────────────────────────────────────────────────────
    // The declare pass may visit a foreign function once per module that
    // imports it. The first visit creates the prototype; subsequent visits
    // are no-ops.
    if (program.lookupFunction(decl)) return;

    // ─── Symbol name ──────────────────────────────────────────────────────
    std::string symbolName = decl->mangledName.isValid()
        ? program.pool.lookup(decl->mangledName)
        : program.pool.lookup(decl->name);
    if (symbolName.empty()) {
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, decl->loc,
            "foreign function declaration has no symbol name");
        return;
    }

    // ─── Signature ────────────────────────────────────────────────────────
    // A foreign function is always `fn`-shaped. `cls`-shaped foreign
    // functions are nonsensical (there's no LLVM-level closure ABI in C),
    // so Sema rejects them before we get here.
    llvm::FunctionType* fnTy = program.types().functionType(
        decl->funcType, /*isClosure=*/false);
    if (!fnTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, decl->loc,
            "foreign function '", program.pool.lookup(decl->name),
            "' has an invalid signature");
        return;
    }

    // ─── Create the external declaration ──────────────────────────────────
    // `ExternalLinkage` and no body. The symbol is resolved at link time
    // (AOT) or JIT time (interpreter) by the host runtime.
    llvm::Function* fn = llvm::Function::Create(
        fnTy,
        llvm::GlobalValue::ExternalLinkage,
        symbolName,
        program.module());

    // Name parameters for readable IR.
    size_t argIdx = 0;
    for (FuncTypeAST* stage = decl->funcType; stage; stage = stage->getNext()) {
        for (ParamAST* param : stage->params) {
            if (argIdx >= fn->arg_size()) break;
            if (!param->name.isValid()) {
                ++argIdx;
                continue;
            }
            fn->getArg(argIdx++)->setName(program.pool.lookup(param->name));
        }
    }

    program.storeFunction(decl, fn);

    Trace::detail("Declared foreign function '", symbolName, "'");
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFuncBody — the body of a `fn`-shaped function
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── The RAII Shape ───────────────────────────────────────────────────────
// A `FunctionState` is constructed on the stack at the top of the body and
// destroyed at the end. Its constructor captures the enclosing function's
// scalar state, saves the builder's insertion point, and installs the new
// state. Its destructor restores everything.
//
// Using the stack form rather than `ProgramState::setCurrentFunctionState`
// keeps the lifetime bound to this one function call, which is exactly
// what the RAII design promises. `setCurrentFunctionState` exists for the
// module pass, where construction and destruction span different scopes.

void Emitter::emitFuncBody(FuncDeclAST* decl) {
    assert(decl && "emitFuncBody() with null declaration");

    llvm::Function* fn = program.lookupFunction(decl);
    if (!fn) return;

    // ─── Get the body ─────────────────────────────────────────────────────
    // The body lives on the declaration's `init` expression. For a
    // block-body function declaration, `init` is an `AnonFuncExprAST`
    // whose `funcType` carries the runtime parameters and whose `body`
    // is the statement tree.
    //
    // A reference-body function (`const f (int) -> int = g;`) has no body
    // of its own; `init` is an identifier or call. That case has no IR to
    // emit here — the reference target already exists.
    AnonFuncExprAST* anon = decl->init && decl->init->isa<AnonFuncExprAST>()
        ? decl->init->as<AnonFuncExprAST>()
        : nullptr;
    if (!anon) return;
    if (!anon->body) return;

    // ─── Guard: no nested function state ──────────────────────────────────
    // A `fn`-shaped function body is lowered either at module scope (no
    // enclosing function) or, in the case of a nested function declaration
    // that's somehow reached this path, inside an enclosing function.
    // The latter is unusual; the current language doesn't have nested
    // named functions, so this path should never fire. If it does, the
    // RAII constructor handles the save/restore correctly anyway.
    //
    // We don't assert `!program.currentFunctionState` here because the
    // RAII design supports nesting. If a future revision adds nested
    // named functions, this path still works.

    // ─── Enter the function state ─────────────────────────────────────────
    // The declared return type comes from `decl->funcType->returnType`.
    // The anon's `funcType` has the same return type by construction, but
    // `decl->funcType` is the source of truth for "what does this function
    // claim to return" — it's what call sites coerced to.
    FunctionState state(program, fn, decl->funcType ? decl->funcType->returnType
                                                     : nullptr);

    // ─── Create the entry block ───────────────────────────────────────────
    llvm::IRBuilder<>& b = program.builder();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(program.llvmContext(), "entry", fn);
    b.SetInsertPoint(entry);

    // ─── Push the function-level scope ────────────────────────────────────
    // Parameters and locals live in this scope. Scope-exit cleanup at
    // function exit releases whatever's still alive.
    func().pushScope();

    // ─── Allocate parameters ──────────────────────────────────────────────
    //
    // ─── Which `funcType` Carries the Runtime Parameters ──────────────────
    // The RUNTIME parameters live on `anon->funcType`, not on
    // `decl->funcType`. See the long comment on `AnonFuncExprAST` in
    // ExprAST.hpp: a `FuncDeclAST`'s `funcType` carries type-only
    // `ParamAST` nodes, while the anon's `funcType` carries the real
    // parameters that body identifiers resolve against. Iterating
    // `decl->funcType` here would bind the wrong `ParamAST` nodes, and
    // every body identifier would fail to find its alloca.
    //
    // The two signatures have the same parameter count and types by
    // construction, so the argument indices line up with the LLVM function.
    size_t argIdx = 0;
    for (FuncTypeAST* stage = anon->funcType; stage; stage = stage->getNext()) {
        for (ParamAST* param : stage->params) {
            if (argIdx >= fn->arg_size()) break;

            llvm::Value* arg = fn->getArg(argIdx++);
            llvm::Type* paramTy = program.types().get(param->type);
            if (!paramTy) {
                program.diagnostics.errorAt(
                    DiagCode::Backend_InvalidIR, param->loc,
                    "parameter '", program.pool.lookup(param->name),
                    "' has an unresolvable type");
                continue;
            }

            // A parameter's storage is a stack slot. This makes `&param`
            // work and keeps the rest of the emitter uniform: every
            // binding's storage is a pointer.
            llvm::AllocaInst* alloca = createEntryAlloca(
                paramTy, program.pool.lookup(param->name));
            if (!alloca) continue;

            b.CreateStore(arg, alloca);
            func().storeValue(param, alloca);

            // A resource-owning parameter takes over the caller's claim.
            // The caller retained on argument pass (see `emitCall`), so
            // the callee releases at function exit. Marking the binding
            // alive is what makes scope-exit cleanup emit that release.
            if (classifyResource(param) != ResourceKind::None) {
                func().markAlive(param);
            }
        }
    }

    // ─── Emit the body ────────────────────────────────────────────────────
    // The body is either a block (the common case) or a single return
    // statement (an `AnonFuncExprAST` whose body was parsed as a lone
    // return, without braces). Sema normalizes the two shapes.
    if (anon->body->isa<BlockStmtAST>()) {
        emitBlock(anon->body->as<BlockStmtAST>());
    } else if (anon->body->isa<ReturnStmtAST>()) {
        emitReturnStmt(anon->body->as<ReturnStmtAST>());
    }

    // ─── Fall-through cleanup for the parameter scope ─────────────────────
    // If the body ended in a terminator (an explicit `return`), its unwind
    // already released every live binding, and `emitScopeFallthrough` is
    // a no-op. If the body fell through (a void function with no explicit
    // return), the fall-through cleanup releases any live parameters
    // before the implicit `ret void`.
    //
    // This is why the order is: body, then scope cleanup, then the
    // fallback terminator. Reversing it would emit the terminator first,
    // and `emitScopeFallthrough` would be appending to a terminated block.
    emitScopeFallthrough();
    func().popScope();

    // ─── Fallback terminator for void functions ───────────────────────────
    // A function that fell through without a terminator needs a `ret`.
    // A non-void function that fell through is a Sema bug (missing return
    // was not caught), so we emit `ret undef` to keep the IR well-formed.
    llvm::BasicBlock* cur = b.GetInsertBlock();
    if (cur && !cur->getTerminator()) {
        llvm::Type* retTy = fn->getReturnType();
        if (retTy->isVoidTy()) {
            b.CreateRetVoid();
        } else {
            // Sema should have rejected a missing return. This is a
            // safety net so the IR verifies; the diagnostic surfaces
            // upstream.
            b.CreateRet(llvm::UndefValue::get(retTy));
        }
    }

    Trace::detail("Lowered body of function '",
                  program.pool.lookup(decl->name), "'");

    // `state`'s destructor restores the enclosing function state and the
    // builder's insertion point as this function returns.
}

// ─────────────────────────────────────────────────────────────────────────────
// emitStructDecl — force the LLVM struct type to exist
// ─────────────────────────────────────────────────────────────────────────────
//
// In the new design, `Types` creates LLVM struct types lazily and caches
// them. `emitStructDecl` is where the declare pass forces creation, so
// that:
//
//   1. A field type that references a not-yet-declared struct (a forward
//      reference) sees an opaque type on its first pass and the concrete
//      body on the second — but the *pointer* is stable, which is what
//      matters.
//
//   2. A diagnostic about a malformed struct surfaces during the declare
//      pass rather than during the first use site.
//
// The work itself is `program.types().structType(decl)`. The emitter's
// contribution is ordering and logging.

void Emitter::emitStructDecl(StructDeclAST* decl) {
    assert(decl && "emitStructDecl() with null declaration");

    // ─── Generic templates are skipped ────────────────────────────────────
    // A generic struct is a family, not a concrete type. Sema produced a
    // specialization for every concrete instantiation, and those
    // specializations are lowered as their own declarations. The template
    // has no LLVM type of its own.
    if (decl->isGeneric()) return;

    // ─── Force the LLVM type to exist ─────────────────────────────────────
    // `Types::structType` is idempotent: on a first call it creates the
    // opaque struct, caches it, and fills the body; on a second call it
    // returns the cached pointer. `decl->llvmType` is set as a side effect
    // (Types still caches on the AST for now).
    llvm::StructType* structTy = program.types().structType(decl);
    if (!structTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, decl->loc,
            "failed to create LLVM struct for '",
            program.pool.lookup(decl->name), "'");
        return;
    }

    Trace::detail("Lowered struct '", program.pool.lookup(decl->name),
                  "' (", decl->fields.size(), " field(s))");
}

// ─────────────────────────────────────────────────────────────────────────────
// emitEnumDecl — force the LLVM integer type to exist
// ─────────────────────────────────────────────────────────────────────────────
//
// An enum lowers to a bare integer of its backing type's width. Two enums
// with the same backing type share the same `llvm::IntegerType*` (LLVM
// interns integer types by bit width per context). So there is no per-enum
// LLVM type to create — the "type" is just the integer type.
//
// What `emitEnumDecl` does:
//
//   1. Resolves the backing integer type via `Types::enumType`.
//   2. Caches it on `decl->backingLLVMType` for later reads.
//   3. Computes the byte size for layout purposes.
//
// Unlike the old `lowerEnumDecl`, it does NOT materialize per-variant
// `ConstantInt`s. Variants are lowered on demand by `emitIdentifier`'s
// `EnumVariantAST` branch, which reads `variant->value` and the resolved
// type. Caching constants on the AST was an optimization for the old
// design; the new design prefers laziness.

void Emitter::emitEnumDecl(EnumDeclAST* decl) {
    assert(decl && "emitEnumDecl() with null declaration");

    // ─── Resolve the backing integer type ─────────────────────────────────
    // `Types::enumType` reads `decl->backingType` if Sema set it, and
    // falls back to i32 if not. The result is a cached integer type.
    llvm::IntegerType* backingTy = program.types().enumType(decl);
    if (!backingTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, decl->loc,
            "failed to determine backing type for enum '",
            program.pool.lookup(decl->name), "'");
        return;
    }
    decl->backingLLVMType = backingTy;

    // ─── Cache the byte size ──────────────────────────────────────────────
    // Used by `sizeof` and by layout calculations. `getTypeAllocSize` is
    // the alloc size (includes padding to alignment), which is what
    // aggregate layout uses.
    const llvm::DataLayout& dl = program.module().getDataLayout();
    decl->byteSize = dl.getTypeAllocSize(backingTy).getFixedValue();

    Trace::detail("Lowered enum '", program.pool.lookup(decl->name),
                  "' (", decl->variants.size(), " variant(s), ",
                  decl->byteSize, " byte(s))");
}

} // namespace codegen