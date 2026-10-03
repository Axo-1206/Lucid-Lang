/// @file Sema.cpp
/// @brief The driver: runs the three Sema passes over a module set.
///
/// ─── The three passes ─────────────────────────────────────────────────────
/// Sema runs in three passes, and every module in the set completes a
/// pass before any module starts the next one. The interleaving is what
/// makes cross-module forward references work: by the time a module's
/// pass 2 resolves a signature that names `weapons.Item`, `weapons`
/// already has its `Item` registered from pass 1.
///
///   Pass 1 — registerModuleDeclarations:
///     Walk each module's `decls`, insert every top-level name into the
///     module's symbol table. No types resolved, no bodies analyzed.
///
///   Pass 2 — resolveModuleDeclarations:
///     Walk each module's `decls` and resolve each declaration's
///     signature or type. `TABLE` columns are resolved, `FN` parameter
///     and return types are resolved, top-level `let`/`const`
///     initializers are resolved. Attribute validation runs here, so
///     `@sequence` is known before pass 3 sees the body.
///
///   Pass 3 — resolveModuleBodies:
///     Walk each module's `decls` and resolve the body of every
///     Lucid-bodied `FN`. By this point every name a body can reference
///     is registered and every type it can mention is resolved.
///
/// ─── Why the driver lives here, not in the CLI ────────────────────────────
/// The passes are not independent: pass 2 assumes pass 1 finished
/// everywhere, pass 3 assumes pass 2 finished everywhere. Whichever
/// caller runs the passes has to know that ordering. Putting the
/// ordering in the CLI would mean every caller (a test, an LSP, a build
/// tool) re-implements it. Keeping it here means a caller says "run
/// Sema over these modules" and the ordering is the same everywhere.
///
/// A caller that owns exactly one module calls `resolveModule`, which
/// runs all three passes over that one module. A caller that owns a set
/// calls `analyze`, which runs each pass across the whole set in order.
///
/// ─── What is not here ─────────────────────────────────────────────────────
/// The set of modules is not chosen here — that is a policy decision
/// (which modules are Tier 1, which are Tier 2, what order mods load
/// in) that belongs to the host. `analyze` runs over whatever span of
/// modules it is given, in the order it is given.

#include "Sema.hpp"

#include "context/SemaContext.hpp"

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// analyze — run all three passes over a module set
// ─────────────────────────────────────────────────────────────────────────────

void analyze(const std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    // ─── Pass 1: register every top-level name ──────────────────────────
    //
    // Every module completes this pass before any module starts pass 2.
    // A module's pass-1 work is a pure function of that module's
    // `decls` span — no cross-module information is consulted — so the
    // per-module order within the pass does not matter for correctness.
    // It is run in the given order anyway, so diagnostics from a
    // predictable sequence of modules come out in the same order each
    // run.
    for (ModuleAST* module : modules) {
        if (!module) continue;
        ctx.enterModule(module);
        registerModuleDeclarations(module, ctx);
        if (!ctx.diagnostics.canContinue()) return;
    }

    // ─── Pass 2: resolve signatures, types, and top-level initializers ──
    //
    // Now every module's top-level names are visible from every other
    // module (through the import aliases registered in pass 1). This
    // pass resolves the signatures of tables and functions, and the
    // initializers of top-level `let`/`const`.
    for (ModuleAST* module : modules) {
        if (!module) continue;
        ctx.enterModule(module);
        resolveModuleDeclarations(module, ctx);

        // A module's `hasErrors` flag records whether any diagnostic was
        // emitted while this module was the active one. It is a
        // snapshot, not a live counter — a later pass over a different
        // module does not change it.
        module->hasErrors = ctx.diagnostics.hasErrors();

        if (!ctx.diagnostics.canContinue()) return;
    }

    // ─── Pass 3: resolve function bodies ────────────────────────────────
    //
    // By this point a body's references to other declarations are
    // resolved through the module table; the body pass only walks
    // expressions and statements.
    for (ModuleAST* module : modules) {
        if (!module) continue;
        ctx.enterModule(module);
        resolveModuleBodies(module, ctx);

        module->hasErrors = ctx.diagnostics.hasErrors();
        if (!ctx.diagnostics.canContinue()) return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveModule — one-module convenience
// ─────────────────────────────────────────────────────────────────────────────

void resolveModule(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    ctx.enterModule(module);
    registerModuleDeclarations(module, ctx);
    if (!ctx.diagnostics.canContinue()) return;

    resolveModuleDeclarations(module, ctx);
    module->hasErrors = ctx.diagnostics.hasErrors();
    if (!ctx.diagnostics.canContinue()) return;

    resolveModuleBodies(module, ctx);
    module->hasErrors = ctx.diagnostics.hasErrors();
}

// ─────────────────────────────────────────────────────────────────────────────
// registerModuleDeclarations — pass 1
// ─────────────────────────────────────────────────────────────────────────────

void registerModuleDeclarations(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;
    if (module->hasSyntaxError) return;

    for (DeclAST* decl : module->decls) {
        if (!decl) continue;
        if (decl->hasSyntaxError) continue;

        switch (decl->kind) {
            case ASTKind::ImportDecl:
                registerImportName(decl->as<ImportDeclAST>(), ctx);
                break;

            case ASTKind::TableDecl:
                registerTableName(decl->as<TableDeclAST>(), ctx);
                break;

            case ASTKind::FnDecl:
                registerFnName(decl->as<FnDeclAST>(), ctx);
                break;

            case ASTKind::VarDecl:
                registerVarName(decl->as<VarDeclAST>(), ctx);
                break;

            default:
                // A module's top level can only contain the four kinds
                // above; anything else is a parser bug, not a user
                // error. The dispatch above covers every kind the parser
                // produces for a top-level decl.
                AST_ASSERT_MSG(false,
                    "registerModuleDeclarations: unexpected top-level "
                    "declaration kind");
                break;
        }

        if (!ctx.diagnostics.canContinue()) return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveModuleDeclarations — pass 2
// ─────────────────────────────────────────────────────────────────────────────

void resolveModuleDeclarations(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;
    if (module->hasSyntaxError) return;

    for (DeclAST* decl : module->decls) {
        if (!decl) continue;

        // A declaration whose name was not registered in pass 1 (a
        // syntax error, or a redeclaration) is skipped. Its name is not
        // in the module table, so anything that references it will
        // already have failed to resolve; running pass 2 on it would
        // only produce follow-on diagnostics.
        if (decl->hasSyntaxError) continue;

        switch (decl->kind) {
            case ASTKind::ImportDecl:
                resolveImportDecl(decl->as<ImportDeclAST>(), ctx);
                break;

            case ASTKind::TableDecl:
                resolveTableDecl(decl->as<TableDeclAST>(), ctx);
                break;

            case ASTKind::FnDecl:
                resolveFnDecl(decl->as<FnDeclAST>(), ctx);
                break;

            case ASTKind::VarDecl:
                resolveVarDecl(decl->as<VarDeclAST>(), ctx);
                break;

            default:
                AST_ASSERT_MSG(false,
                    "resolveModuleDeclarations: unexpected top-level "
                    "declaration kind");
                break;
        }

        if (!ctx.diagnostics.canContinue()) return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveModuleBodies — pass 3
// ─────────────────────────────────────────────────────────────────────────────

void resolveModuleBodies(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;
    if (module->hasSyntaxError) return;

    for (DeclAST* decl : module->decls) {
        if (!decl) continue;
        if (decl->hasSyntaxError) continue;

        // Only functions have bodies. Tables and variables are fully
        // resolved in pass 2 — a table's initializer rows are const
        // expressions evaluated at pass 2 time, and a top-level
        // variable's initializer is resolved in pass 2 because a
        // variable has no separate body.
        if (!decl->isa<FnDeclAST>()) continue;

        FnDeclAST* fn = decl->as<FnDeclAST>();

        // A host-bound function has no body to resolve.
        if (fn->isHostBound) continue;

        resolveFnBody(fn, ctx);

        if (!ctx.diagnostics.canContinue()) return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveDecl — per-declaration dispatcher
// ─────────────────────────────────────────────────────────────────────────────
//
// Public for the LSP and for tests. The three module passes call the
// per-kind resolvers directly rather than going through this function,
// because a module pass knows which pass it is and can assert on it —
// `resolveModuleDeclarations` never dispatches a `FnDeclAST` to a
// body resolver, and `resolveModuleBodies` never dispatches a
// `TableDeclAST` to a signature resolver. This function exists for the
// caller that wants to resolve a single declaration without knowing
// which pass it belongs to.
//
// "Which pass it belongs to" is not a property of the declaration kind
// alone: a top-level `let` is resolved in pass 2, but a local `let` is
// resolved in pass 3 by `SemaStmt`'s `resolveVarDeclStmt`. This
// dispatcher handles the top-level form. A caller that resolves a
// declaration appearing inside a block should route through
// `resolveStmt` instead.

void resolveDecl(DeclAST* decl, SemaContext& ctx) {
    if (!decl) return;
    if (decl->hasSyntaxError) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl:
            resolveImportDecl(decl->as<ImportDeclAST>(), ctx);
            return;

        case ASTKind::TableDecl:
            resolveTableDecl(decl->as<TableDeclAST>(), ctx);
            return;

        case ASTKind::FnDecl: {
            FnDeclAST* fn = decl->as<FnDeclAST>();
            resolveFnDecl(fn, ctx);

            // If the function has a body, resolve it. `resolveDecl` is
            // the "resolve this declaration completely" entry point;
            // the passes call the signature and body resolvers
            // separately, but a tool that resolves a single declaration
            // wants both.
            if (!fn->isHostBound && !fn->hasSyntaxError) {
                resolveFnBody(fn, ctx);
            }
            return;
        }

        case ASTKind::VarDecl:
            // Top-level form. The caller is responsible for having
            // registered the name in pass 1. A local `let` reaches
            // `resolveVarDecl` from `resolveVarDeclStmt`, which
            // registers the name itself.
            resolveVarDecl(decl->as<VarDeclAST>(), ctx);
            return;

        default:
            // A per-declaration entry point that gets a kind it does
            // not handle is a caller bug — pass the right node, or
            // route through the appropriate pass.
            AST_ASSERT_MSG(false,
                "resolveDecl: unexpected declaration kind");
            return;
    }
}

} // namespace lucid::sema