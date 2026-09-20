/// @file codegen/emit/EmitDecl.cpp
/// @brief Declaration lowering — the `Emitter::emit(DeclAST*)` entry point
///        and its per-kind dispatch.
///
/// ─── Two Phases ───────────────────────────────────────────────────────────
/// Declarations are emitted in two phases by the pass runner:
///
///   1. Declare pass — every function gets a prototype; every struct/enum
///      gets its LLVM type. No bodies. This is what makes forward
///      references resolvable.
///
///   2. Define pass — every function body is lowered. Local variables are
///      allocated and initialized.
///
/// `emit(DeclAST*)` is called in both phases. For a function declaration,
/// the emitter creates the prototype if one doesn't exist, then (in the
/// define pass) emits the body. The order of calls determines the phase.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/support/CodeGenPanic.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
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
        case ASTKind::TraitDecl:  return;  // compile-time only
        case ASTKind::ImportDecl: return;  // resolved by module loader

        default:
            return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitVarDecl — local variable declaration
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Module-Level vs. Local ───────────────────────────────────────────────
// A module-level variable's storage is a field in the module instance
// struct (emitted by the module pass in Task 7). `emitVarDecl` is a
// no-op for module-level bindings — the initializer runs in
// `__init_module_<name>`, and the release runs in `__free_module_<name>`.
//
// A local variable gets an alloca and its initializer runs here.

void Emitter::emitVarDecl(VarDeclAST* decl) {
    assert(decl && "emitVarDecl() with null declaration");

    // Module-level bindings are handled by the module pass. Skip.
    if (decl->isModuleLevel()) return;

    llvm::IRBuilder<>& b = program.builder();

    // ─── Allocate storage ─────────────────────────────────────────────────
    llvm::Type* varTy = program.types().get(decl->type);
    if (!varTy) return;

    llvm::AllocaInst* alloca = b.CreateAlloca(
        varTy, nullptr, program.pool.lookup(decl->name));

    // ─── Register the binding ─────────────────────────────────────────────
    // The value map is keyed on the declaration and holds the alloca.
    // Note: the binding is NOT marked alive yet — the initializer will
    // do that when it stores into the place, via `store`'s `markAlive`.
    func().storeValue(decl, alloca);

    // ─── Emit the initializer, if any ─────────────────────────────────────
    if (decl->init) {
        Val initVal = emit(decl->init);
        if (!initVal.isValid()) return;

        // Coerce to the declared type (fn→cls widening, integer
        // narrowing/widening, pointer casts).
        initVal = coerceTo(initVal, decl->type, *this);
        if (!initVal.isValid()) return;

        // Store through the single write path. `store` will:
        //   - call intoOwned on the init value,
        //   - drop the old value (which is undef — the binding isn't
        //     alive yet, so the drop is skipped),
        //   - store the new value,
        //   - mark the binding alive.
        Place place{alloca, decl->type};
        store(place, initVal, decl);
    } else {
        // No initializer. For a resource-owning binding, this is a bug —
        // Sema should have required an initializer or defaulted one. For
        // a non-resource binding, the alloca is fine as-is; a load before
        // the first store would read undef, which is a Sema-rejected
        // program, so we don't worry about it.
        //
        // A nullable/fallible binding without an initializer gets a
        // default-init expression from Sema (an UnknownExprAST with the
        // sentinel tag). The emitter's `emit(UnknownExprAST*)` path is
        // not written here; it's part of the general emitter body. For
        // now, trust Sema.
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFuncDecl — function prototype or body, depending on phase
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitFuncDecl(FuncDeclAST* decl) {
    assert(decl && "emitFuncDecl() with null declaration");

    // ─── Foreign functions ────────────────────────────────────────────────
    // A foreign function is a bare `declare`. No body.
    if (decl->isForeignFunction) {
        emitForeignFuncDecl(decl);
        return;
    }

    // ─── `cls`-shaped functions ───────────────────────────────────────────
    // A `cls`-shaped function is a runtime fat pointer, not an LLVM
    // function. The fat pointer is constructed during the define pass,
    // and the body is lowered as a closure body.
    FuncShape shape = decl->funcType ? decl->funcType->shape : FuncShape::Fn;
    if (shape == FuncShape::Cls) {
        emitClosureFuncDecl(decl);
        return;
    }

    // ─── `fn`-shaped functions ────────────────────────────────────────────
    // Get or create the LLVM function prototype. If it doesn't exist,
    // create it. Then emit the body if it hasn't been emitted yet.
    llvm::Function* fn = program.lookupFunction(decl);
    if (!fn) {
        // Declare pass: create the prototype.
        std::string name = program.pool.lookup(decl->mangledName);
        llvm::FunctionType* fnTy = program.types().functionType(
            decl->funcType, /*isClosure=*/false);
        if (!fnTy) return;

        llvm::GlobalValue::LinkageTypes linkage =
            decl->isExported ? llvm::GlobalValue::ExternalLinkage
                             : llvm::GlobalValue::InternalLinkage;

        fn = llvm::Function::Create(fnTy, linkage, name, program.module());
        program.storeFunction(decl, fn);

        // Name the parameters.
        size_t argIdx = 0;
        for (FuncTypeAST* stage = decl->funcType; stage; stage = stage->getNext()) {
            for (ParamAST* param : stage->params) {
                if (argIdx < fn->arg_size()) {
                    fn->getArg(argIdx++)->setName(
                        program.pool.lookup(param->name));
                }
            }
        }
    }

    // ─── Define pass: emit the body if not emitted ────────────────────────
    if (fn->empty()) {
        emitFuncBody(decl);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFuncBody — the body of a `fn`-shaped function
// ─────────────────────────────────────────────────────────────────────────────

void Emitter::emitFuncBody(FuncDeclAST* decl) {
    assert(decl && "emitFuncBody() with null declaration");

    llvm::Function* fn = program.lookupFunction(decl);
    if (!fn) return;

    // ─── Get the body ─────────────────────────────────────────────────────
    // The body lives on the declaration's `init` expression, which is an
    // `AnonFuncExprAST`. A reference-body function (init is not an anon)
    // has no body of its own.
    AnonFuncExprAST* anon = decl->init && decl->init->isa<AnonFuncExprAST>()
        ? decl->init->as<AnonFuncExprAST>()
        : nullptr;
    if (!anon) return;

    // ─── Enter the function state ─────────────────────────────────────────
    // Constructing a `FunctionState` saves the enclosing state, installs
    // the new one, and pushes the parameter scope.
    program.setCurrentFunctionState(fn, decl->funcType->returnType);

    // ─── Create the entry block ───────────────────────────────────────────
    llvm::IRBuilder<>& b = program.builder();
    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(program.llvmContext(), "entry", fn);
    b.SetInsertPoint(entry);

    // ─── Push the function's scope ────────────────────────────────────────
    // The parameters and locals live in this scope. Its cleanup at
    // function exit releases whatever's still alive.
    func().pushScope();

    // ─── Allocate parameters ──────────────────────────────────────────────
    // Each parameter gets an alloca; the incoming argument value is
    // stored into it. This makes `&param` work and matches the old
    // behavior.
    //
    // Parameters that own resources are marked alive so cleanup releases
    // them.
    size_t argIdx = 0;
    for (FuncTypeAST* stage = decl->funcType; stage; stage = stage->getNext()) {
        for (ParamAST* param : stage->params) {
            if (argIdx >= fn->arg_size()) break;

            llvm::Value* arg = fn->getArg(argIdx++);
            llvm::Type* paramTy = program.types().get(param->type);
            if (!paramTy) continue;

            llvm::AllocaInst* alloca = b.CreateAlloca(
                paramTy, nullptr, program.pool.lookup(param->name));
            b.CreateStore(arg, alloca);
            func().storeValue(param, alloca);

            if (ownsResource(param)) {
                func().markAlive(param);
            }
        }
    }

    // ─── Emit the body ────────────────────────────────────────────────────
    if (anon->body) {
        if (anon->body->isa<BlockStmtAST>()) {
            emitBlock(anon->body->as<BlockStmtAST>());
        } else if (anon->body->isa<ReturnStmtAST>()) {
            emitReturnStmt(anon->body->as<ReturnStmtAST>());
        }
    }

    // ─── Pop the function scope ───────────────────────────────────────────
    // If the block ends in a terminator (return, break), the cleanup
    // was already emitted by the unwind. If not, this pop emits the
    // fall-through cleanup.
    //
    // NOTE: the cleanup emission is part of the emitter's scope
    // management. Task 5's emitter will move it out of the shim.
    func().popScope();

    // ─── Emit a fall-through terminator if needed ─────────────────────────
    llvm::BasicBlock* cur = b.GetInsertBlock();
    if (cur && !cur->getTerminator()) {
        llvm::Type* retTy = fn->getReturnType();
        if (retTy->isVoidTy()) {
            b.CreateRetVoid();
        } else {
            // Sema should have rejected missing returns. This is a
            // safety net so the IR is well-formed.
            b.CreateRet(llvm::UndefValue::get(retTy));
        }
    }

    // ─── Leave the function state ─────────────────────────────────────────
    program.clearCurrentFunctionState();
}

// ... emitForeignFuncDecl, emitClosureFuncDecl, emitStructDecl,
// emitEnumDecl ...

} // namespace codegen