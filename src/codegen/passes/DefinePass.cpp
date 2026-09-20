/// @file codegen/passes/DefinePass.cpp
/// @brief Function body emission.

#include "Passes.hpp"

#include "codegen/Emitter.hpp"
#include "codegen/Program.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/ast/ModuleAST.hpp"
#include "core/trace/Trace.hpp"

namespace codegen {

namespace {

/// Emit a function body, if it hasn't been emitted yet.
void defineFunction(FuncDeclAST* decl, Emitter& emitter, ProgramState& program) {
    if (!decl || decl->hasSyntaxError) return;

    // Skip foreign functions (no body).
    if (decl->isForeignFunction) return;

    // Skip generic templates; specializations are emitted below.
    if (decl->isGeneric()) return;

    // `cls`-shaped functions are values, not LLVM functions; their
    // "body" is the closure body, emitted by the emitter's closure path.
    FuncShape shape = decl->funcType ? decl->funcType->shape : FuncShape::Fn;
    if (shape == FuncShape::Cls) {
        // The declare pass didn't create a prototype for cls-shaped
        // functions. The define pass constructs the fat pointer by
        // emitting the function as a closure.
        emitter.emit(decl);
        return;
    }

    // Non-foreign, non-generic, fn-shaped: emit the body.
    emitter.emit(decl);
}

void define(DeclAST* decl, Emitter& emitter, ProgramState& program) {
    if (!decl || decl->hasSyntaxError) return;

    switch (decl->kind) {
        case ASTKind::FuncDecl:
            defineFunction(decl->as<FuncDeclAST>(), emitter, program);
            break;
        case ASTKind::VarDecl:
            // Module-level variables are handled by ModulePass.
            // Local variables are handled as part of the enclosing
            // function body.
            break;
        default:
            break;
    }
}

} // anonymous namespace

void runDefinePass(const std::vector<ModuleAST*>& modules,
                   ProgramState& program,
                   Manifest& manifest) {
    Trace::info("DefinePass: ", modules.size(), " modules");

    Emitter& emitter = program.emitter();

    for (ModuleAST* module : modules) {
        if (!module) continue;

        program.currentModule = module;

        // ─── 1. Bodies of parser-produced functions ───────────────────────
        for (DeclAST* decl : module->decls) {
            define(decl, emitter, program);
        }

        // ─── 2. Bodies of specializations ─────────────────────────────────
        for (DeclAST* spec : module->specializations) {
            define(spec, emitter, program);
        }
    }

    // ─── 3. Manifest: find the entry symbol ───────────────────────────────
    // The entry is `@[export] const main`. Its mangled name is what the
    // host uses to invoke the program.
    for (ModuleAST* module : modules) {
        if (!module) continue;
        for (DeclAST* decl : module->decls) {
            if (!decl->isa<FuncDeclAST>()) continue;
            FuncDeclAST* fn = decl->as<FuncDeclAST>();
            if (!fn->isExported) continue;
            if (program.pool.lookup(fn->name) != "main") continue;

            manifest.entry.symbol = program.pool.lookup(fn->mangledName);
            manifest.entry.isCMain = true;  // for now
            break;
        }
    }

    program.currentModule = nullptr;

    Trace::detail("DefinePass complete");
}

} // namespace codegen