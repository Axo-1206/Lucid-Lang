/**
 * @file Sema.cpp
 * @brief Implements the public API for the semantic phase.
 *
 * @architectural_note Top-Down Type Resolution
 *   Lucid uses a top-down, on-the-fly type resolution strategy. Types are
 *   resolved immediately as declarations are analyzed, with one exception:
 *   structs require a two-pass approach to support self-reference.
 *
 * @architectural_note Struct Two-Pass Analysis
 *   Structs are the only construct that require a separate pass:
 *     1. PASS 1: Register struct name and all field names
 *     2. PASS 2: Resolve field types (now self-reference is possible)
 *
 * @architectural_note Const Evaluation
 *   Const evaluation is a separate phase that runs AFTER all type resolution.
 *   This ensures all types are fully resolved and all declarations are
 *   registered before evaluating const expressions.
 * 
 * analyze(modules, ctx)
 *    │
 *    ├── PHASE 1: Declaration Registration (Top-Down)
 *    │   └── For each module:
 *    │       └── analyzeModuleDecls(module, ctx)
 *    │           └── For each decl: analyzeDecl(decl, ctx)
 *    │               ├── VarDecl: resolve type, check init, register
 *    │               ├── FuncDecl: resolve type, register, analyze body
 *    │               ├── EnumDecl: register, analyze variants
 *    │               ├── TraitDecl: register, analyze fields
 *    │               └── StructDecl:
 *    │                   ├── Register struct name
 *    │                   ├── Register generic params
 *    │                   ├── PASS 1: Register all field names
 *    │                   ├── PASS 2: Resolve field types
 *    │                   └── Analyze function field bodies
 *    │
 *    └── PHASE 2: Const Evaluation
 *        └── evaluateConstDeclarations(modules, ctx)
 *            └── ConstEvaluator.evaluateAll()
 *                └── Evaluate const expressions (after all types are resolved)
 *                
 */

#include "Sema.hpp"
#include "context/SemaContext.hpp"
#include "const_eval/ConstEvaluator.hpp"

namespace sema {

/**
 * @brief Analyze all modules in the program.
 *
 * Processes every module in `modules` in the order provided.
 *
 * @param modules The modules to analyze (in dependency order - imports first).
 * @param ctx     The semantic context (shared across all modules).
 */
void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 1: Declaration Analysis (Top-Down Type Resolution)
    // ─────────────────────────────────────────────────────────────────────────
    //
    // Walk each module's top-level declarations in source order.
    // Types are resolved immediately as declarations are analyzed.
    //
    // Exception: Structs use a two-pass approach within the struct itself:
    //   - PASS 1: Register all field names (no type resolution)
    //   - PASS 2: Resolve field types (enables self-reference)
    //
    // This is the only construct that requires a separate pass.
    for (ModuleAST* module : modules) {
        if (!module) continue;

        // Enter the module's context
        ctx.symbols.enterModule(module);

        // Walk declarations - this handles all type resolution
        analyzeModuleDecls(module, ctx);

        // Record whether this module had errors.
        module->hasErrors = diagnostic::hasErrorsInCurrentSource();

        // Check if we've hit the fatal-error threshold.
        if (!ctx.canContinue()) {
            return;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 2: Const Evaluation
    // ─────────────────────────────────────────────────────────────────────────
    //
    // Evaluate all const declarations at compile-time.
    // This runs AFTER all type resolution so that:
    //   - All types are fully resolved
    //   - All declarations are registered
    //   - Const dependencies can be resolved
    evaluateConstDeclarations(modules, ctx);

    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 3: Post-Evaluation Verification (Optional)
    // ─────────────────────────────────────────────────────────────────────────
    //
    // Verify that all const values are used correctly.
    // For example, check that const values are not used in contexts where
    // they would be treated as mutable.
    //
    // TODO: Implement post-evaluation verification
}

// =============================================================================
// Module-Level Analysis
// =============================================================================

/**
 * @brief Walk a single module's top-level declarations in source order.
 *
 * Always processes every declaration it can, even after individual
 * declarations report errors. Stops early only if `ctx.canContinue()`
 * becomes false (too many consecutive errors).
 *
 * This function handles all type resolution. Types are resolved immediately
 * as declarations are analyzed.
 *
 * @param module The module whose `decls` span is walked.
 * @param ctx    The semantic context. Caller (`sema::analyze`) is
 *        responsible for having already entered `module` via
 *        `ctx.symbols.enterModule(module)`.
 */
void analyzeModuleDecls(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    // Walk all top-level declarations in source order.
    // This is a single pass: declarations are inserted as they're reached,
    // and lookups only see what has been inserted so far.
    for (DeclAST* decl : module->decls) {
        if (!decl) continue;

        // Process the declaration.
        // analyzeDecl() dispatches to the specific analyze*Decl() function.
        // Types are resolved immediately (except structs, which use two-pass).
        analyzeDecl(decl, ctx);

        // Check if we've hit the fatal-error threshold.
        if (!ctx.canContinue()) {
            return;
        }
    }
}

// =============================================================================
// Const Evaluation
// =============================================================================

/**
 * @brief Evaluate all const declarations in the modules.
 *
 * Called after type checking. Replaces const expressions with their
 * evaluated values (stored in ExprAST::constValue).
 *
 * @param modules The modules to evaluate.
 * @param ctx     The semantic context.
 */
void evaluateConstDeclarations(std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    // Create the const evaluator
    ConstEvaluator evaluator(ctx);

    // Evaluate all const declarations
    evaluator.evaluateAll();

    // Check for errors during evaluation
    if (diagnostic::hasErrors()) {
        // Errors were reported by the evaluator
        // We can continue, but modules may have unresolved const values
        return;
    }

    // TODO: Add post-evaluation verification
}

} // namespace sema