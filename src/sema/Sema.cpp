/**
 * @file Sema.cpp
 * @brief Implements the public API for the semantic phase.
 *
 * @architectural_note Namespace alignment with parser
 *   This follows the same pattern as the parser: a namespace `sema` with a
 *   single entry point `analyze()`, and a context struct `SemaContext` that
 *   holds the shared state. This is cleaner and more consistent than the
 *   previous class-based design.
 *
 * @architectural_note Single entry point
 *   `sema::analyze(modules, ctx)` is the ONE entry point for semantic
 *   analysis. It processes every module in `modules` in dependency order.
 *
 * @architectural_note Per-module isolation via ScopedModuleContext
 *   Each module is analyzed in its own ScopedModuleContext, which:
 *     1. Saves the previous module's state
 *     2. Switches to the new module's persistent ModuleTable
 *     3. Resets all transient state (scopes, context stack, defining-type stack)
 *     4. Opens a diagnostic::ScopedSource for this module
 *     5. Restores everything on destruction
 *
 *   This ensures that transient state from one module never leaks into
 *   another module's analysis, while persistent state (ModuleTable) is
 *   preserved across the whole compilation.
 *
 * @architectural_note Error handling
 *   analyzeModuleDecls() processes every declaration it can, even after
 *   individual declarations report errors. It stops early only if
 *   ctx.canContinue() becomes false (too many consecutive errors),
 *   mirroring the parser's own fatal-failure threshold.
 */

#include "Sema.hpp"
#include "context/SemaContext.hpp"

namespace sema {

/**
 * @brief Analyze all modules in the program.
 *
 * Processes every module in `modules` in the order provided. Each module
 * is wrapped in a ScopedModuleContext to isolate transient state.
 *
 * @param modules The modules to analyze (in dependency order - imports first).
 * @param ctx     The semantic context (shared across all modules).
 */
void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    for (ModuleAST* module : modules) {
        if (!module) continue;

        // Analyze this module in its own context.
        // ScopedModuleContext:
        //   - Saves the previous module's state
        //   - Switches to this module's persistent ModuleTable
        //   - Resets transient state (scopes, context stack, defining-type stack)
        //   - Opens a diagnostic::ScopedSource for this module
        //   - Restores everything on destruction
        ScopedModuleContext moduleContext(ctx, module);

        // Walk the module's top-level declarations in source order.
        analyzeModuleDecls(module, ctx);

        // Record whether this module had errors.
        // diagnostic::hasErrorsInCurrentSource() reads the current diagnostic
        // scope's error count (the one opened by ScopedModuleContext).
        module->hasErrors = diagnostic::hasErrorsInCurrentSource();

        // Check if we've hit the fatal-error threshold.
        if (!ctx.canContinue()) {
            // Too many consecutive errors across modules — stop processing.
            return;
        }
    }
}

// =============================================================================
// Module-Level Analysis (implementation of the internal function)
// =============================================================================

/**
 * @brief Walk a single module's top-level declarations in source order.
 *
 * Always processes every declaration it can, even after individual
 * declarations report errors. Stops early only if `ctx.canContinue()`
 * becomes false (too many consecutive errors).
 *
 * @param module The module whose `decls` span is walked.
 * @param ctx    The semantic context. Caller (`sema::analyze`) is
 *        responsible for having already entered `module` via
 *        ScopedModuleContext — this function assumes `ctx.symbols.currentModule()`
 *        and `ctx.symbols.currentModuleTable()` are already correct.
 */
void analyzeModuleDecls(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    // Walk all top-level declarations in source order.
    // This is a single pass: declarations are inserted as they're reached,
    // and lookups only see what has been inserted so far.
    for (DeclAST* decl : module->decls) {
        if (!decl) continue;

        // Process the declaration.
        // analyzeDecl() dispatches to the specific analyze*Decl() function
        // in sema/rules/SemaDecl.cpp.
        analyzeDecl(decl, ctx);

        // Check if we've hit the fatal-error threshold.
        if (!ctx.canContinue()) {
            // Too many consecutive errors — stop processing.
            // The error has already been reported by ctx.error().
            return;
        }
    }
}

} // namespace sema