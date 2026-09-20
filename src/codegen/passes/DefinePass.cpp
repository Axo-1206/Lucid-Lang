/// @file codegen/passes/DefinePass.cpp
/// @brief Function body emission.

#include "Passes.hpp"

#include "codegen/emit/Emitter.hpp"
#include "codegen/Program.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
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

// ─────────────────────────────────────────────────────────────────────────────
// Entry-Point Discovery
// ─────────────────────────────────────────────────────────────────────────────
//
// The manifest's `entry` field tells the host what to call to start the
// program, and whether it can be called as a C `main` or must be reached
// through the Lucid ABI.
//
// The rule:
//
//   - A function named `main`, marked `@[export]`, with NO parameters and
//     a return type that is either void or an integer, CAN be wrapped by a
//     C `main` shim. `isCMain = true`.
//
//   - Any other `@[export]` function is a library entry. The host must
//     call it as a Lucid function (after calling `__lucid_program_init`)
//     and cannot rely on the C `main` contract. `isCMain = false`.
//
// The C `main` contract is `int(int argc, char** argv)`. A Lucid `main`
// doesn't take those parameters, so a shim is needed either way — the
// difference is whether the *host* provides that shim (AOT links against
// `main` in the C runtime) or the *Lucid program* provides it (the
// interpreter calls the Lucid entry directly).

/// True if the signature can be reached through a C `main` shim.
bool looksLikeCMain(FuncDeclAST* fn) {
    if (!fn || !fn->funcType) return false;

    // No parameters: `() -> T`.
    if (!fn->funcType->params.empty()) return false;

    // No curried stages: `() -> () -> T` is not a C main.
    if (fn->funcType->isCurried()) return false;

    // Return type: void or an integer. Anything else (string, struct,
    // closure, ...) can't be the return of a C `int main()`.
    TypeAST* ret = fn->funcType->returnType;
    if (!ret) return true;   // void

    if (ret->isa<PrimitiveTypeAST>()) {
        PrimitiveKind k = ret->as<PrimitiveTypeAST>()->primitiveKind;
        // Match the C `int` return: any integer primitive is fine; the
        // shim truncates to `int` on the way out.
        return isIntegerKind(k);
    }

    return false;
}

/// Find the program's entry function and populate `manifest.entry`.
///
/// Walks every module's declarations in order. The first `@[export]`
/// function named `main` wins; if there isn't one, the first `@[export]`
/// function of any name is used as a library entry.
void findEntry(const std::vector<ModuleAST*>& modules,
               ProgramState& program,
               Manifest& manifest) {
    FuncDeclAST* mainFn = nullptr;
    FuncDeclAST* firstExported = nullptr;

    for (ModuleAST* module : modules) {
        if (!module) continue;
        for (DeclAST* decl : module->decls) {
            if (!decl->isa<FuncDeclAST>()) continue;
            FuncDeclAST* fn = decl->as<FuncDeclAST>();
            if (!fn->isExported) continue;
            if (fn->hasSyntaxError) continue;

            // Record the first exported function as a fallback.
            if (!firstExported) firstExported = fn;

            // Prefer an exported function actually named `main`.
            if (program.pool.lookupView(fn->name) == "main") {
                mainFn = fn;
                break;
            }
        }
        if (mainFn) break;
    }

    FuncDeclAST* entry = mainFn ? mainFn : firstExported;
    if (!entry) return;   // library with no export — entry stays empty

    if (!entry->mangledName.isValid()) {
        // Sema should always mangle exported functions. If it didn't,
        // fall back to the source name and report.
        program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, entry->loc,
            "exported function '", program.pool.lookup(entry->name),
            "' has no mangled name");
        return;
    }

    manifest.entry.symbol = program.pool.lookup(entry->mangledName);
    manifest.entry.isCMain = (program.pool.lookupView(entry->name) == "main")
                           && looksLikeCMain(entry);
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
    findEntry(modules, program, manifest);

    program.currentModule = nullptr;

    Trace::detail("DefinePass complete");
}

} // namespace codegen