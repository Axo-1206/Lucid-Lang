/// @file Sema.cpp
/// @brief Implements the public API for the semantic phase.
///
/// @architectural_note Two-Pass Name Resolution
///   Lucid now uses a two-pass approach:
///     Phase 1: Register ALL names (no type resolution)
///       - Walk AST and register every name in the symbol table
///       - This includes module-level and all nested scopes
///     Phase 2: Resolve ALL types and check bodies
///       - Walk AST again, now all names are available
///       - Resolve types, check bodies, perform semantic validation
///
/// @architectural_note Struct Two-Pass
///   Structs already used a two-pass approach internally:
///     Phase 1: Register struct name and field names
///     Phase 2: Resolve field types (enables self-reference)
///   This is now naturally aligned with the global two-pass design.

#include "Sema.hpp"
#include "context/SemaContext.hpp"
#include "const_eval/ConstEvaluator.hpp"

namespace sema {

// =============================================================================
// analyze - Main Entry Point
// =============================================================================

void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 1: Register ALL names (No type resolution)
    // ─────────────────────────────────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        if (!module) continue;
        ctx.symbols.enterModule(module);
        registerModuleNames(module, ctx);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 2: Resolve ALL types, check bodies, AND evaluate consts
    // ─────────────────────────────────────────────────────────────────────────
    // 
    // Const evaluation is now INTEGRATED into this phase.
    // No separate Phase 3 is needed.
    for (ModuleAST* module : modules) {
        if (!module) continue;

        ctx.symbols.enterModule(module);
        resolveModuleDecls(module, ctx);

        module->hasErrors = diagnostic::hasErrorsInCurrentSource();

        if (!ctx.canContinue()) {
            return;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // NOTE: Phase 3 (Const Evaluation) has been REMOVED.
    // Const evaluation now happens during Phase 2 in resolveVarDecl().
    // ─────────────────────────────────────────────────────────────────────────
}

} // namespace sema