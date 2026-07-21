/**
 * @file Sema.cpp
 * @brief Implements Sema.hpp's public API — Sema::analyze() and
 *        Sema::analyzeAll().
 *
 * @architectural_note Single entry point, two convenient overloads
 *   Sema::analyze(module, ctx) is the one true entry point for analyzing
 *   a single module. Sema::analyzeAll(ctx) is a convenience that iterates
 *   over ctx.modules in order — useful for callers that don't yet have
 *   real dependency ordering wired up (see ModuleRegistry).
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
#include "SemaContext.hpp"

// =============================================================================
// Public API — Single Entry Point
// =============================================================================

/**
 * @brief Analyze a single module: validate and annotate its AST in place.
 *
 * This is the ONE entry point for analyzing one module. It wraps the
 * whole call in a `ScopedModuleContext`, so `ctx` starts with a clean
 * transient state (empty scope stack, empty semantic-context stack,
 * empty defining-type stack, empty per-module error list) and ends with
 * this module's diagnostics drained into `ctx.allDiagnostics` — exactly
 * once per module, regardless of how many times `analyze()` is called
 * across the whole compilation.
 *
 * @param module The module to analyze. Must be non-null; a module that
 *        failed to parse cleanly is still analyzed (check
 *        `module->hasErrors` beforehand if you want to skip that).
 * @param ctx    The semantic context (shared across every module in
 *        this compilation — see SemaContext's own doc comment).
 *
 * @note Does not decide inter-module ordering. If module A imports
 *       module B, the caller is responsible for analyzing B first (or
 *       for `ctx.registry` already holding B's export table) — this
 *       function only handles what's needed to analyze `module` itself.
 */
void Sema::analyze(ModuleAST* module, SemaContext& ctx) {
    if (!module) {
        return;
    }

    // Wrap the whole analysis in a ScopedModuleContext.
    // This saves the previous module's state, switches to this module's
    // persistent table, resets transient state, and opens a diagnostic
    // scope for this module. All of that is restored when this guard
    // goes out of scope.
    ScopedModuleContext moduleContext(ctx, module);

    // Walk the module's top-level declarations in source order.
    analyzeModuleDecls(module, ctx);

    // Record whether this module had errors.
    // diagnostic::hasErrorsInCurrentSource() reads the current diagnostic
    // scope's error count (the one opened by ScopedModuleContext).
    module->hasErrors = diagnostic::hasErrorsInCurrentSource();
}

/**
 * @brief Analyze every module in `ctx.modules`, in the order stored there.
 *
 * A convenience default for callers that don't yet have real dependency
 * ordering wired up (see ModuleRegistry). Equivalent to calling
 * `analyze()` once per module in `ctx.modules` order. Prefer calling
 * `analyze()` directly, module by module, once real import-dependency
 * ordering exists.
 *
 * @param ctx The semantic context containing the list of modules to analyze.
 */
void Sema::analyzeAll(SemaContext& ctx) {
    for (ModuleAST* module : ctx.modules) {
        if (!module) continue;
        analyze(module, ctx);
    }
}

// =============================================================================
// Module-Level Analysis
// =============================================================================

/**
 * @brief Walk a single module's top-level declarations in source order.
 *
 * Always processes every declaration it can, even after individual
 * declarations report errors — check `ctx.hasErrors`, not a return value,
 * to tell a clean analysis from one that hit errors along the way. Stops
 * early only if `ctx.canContinue()` becomes false (too many consecutive
 * errors), mirroring the parser's own fatal-failure threshold.
 *
 * @param module The module whose `decls` span is walked.
 * @param ctx    The semantic context. Caller (`Sema::analyze`) is
 *        responsible for having already entered `module` via
 *        ScopedModuleContext — this function assumes `ctx.currentModule`
 *        and `ctx.currentModuleTable` are already correct.
 */
void analyzeModuleDecls(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    // Walk all top-level declarations in source order.
    // This is a single pass: declarations are inserted as they're reached,
    // and lookups only see what has been inserted so far.
    for (DeclAST* decl : module->decls) {
        if (!decl) continue;

        // Process the declaration.
        analyzeDecl(decl, ctx);

        // Check if we've hit the fatal-error threshold.
        if (!ctx.canContinue()) {
            // Too many consecutive errors — stop processing.
            // The error has already been reported by ctx.error().
            return;
        }
    }
}
