/// @file Sema.cpp
/// @brief Implements the public API for the semantic phase.

#include "Sema.hpp"
#include "context/SemaContext.hpp"
#include "const_eval/ConstEvaluator.hpp"

namespace sema {

void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 1: Register ALL names (No type resolution)
    // ─────────────────────────────────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        if (!module) continue;
        ctx.enterModule(module);
        registerModuleNames(module, ctx);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 2: Resolve ALL types, check bodies, AND evaluate consts
    // ─────────────────────────────────────────────────────────────────────────
    // 
    // Const evaluation is now INTEGRATED into this phase via resolveVarDecl.
    // No separate const evaluation phase is needed.
    // ─────────────────────────────────────────────────────────────────────────
    
    // Create const evaluator once for the entire phase
    ConstEvaluator evaluator(ctx);

    for (ModuleAST* module : modules) {
        if (!module) continue;

        ctx.enterModule(module);
        resolveModuleDecls(module, ctx);

        module->hasErrors = diagnostic::hasErrorsInCurrentSource();

        if (!ctx.canContinue()) {
            return;
        }
    }
    
    // Note: Const evaluation happens during resolveVarDecl, not as a separate phase.
    // The ConstEvaluator instance is used by resolveVarDecl when it sees const declarations.
}

} // namespace sema