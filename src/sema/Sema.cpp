/// @file Sema.cpp
/// @brief Implements the public API for the semantic phase.

#include "Sema.hpp"
#include "context/SemaContext.hpp"
#include "const_eval/ConstEvaluator.hpp"
#include "core/ast/BaseAST.hpp"

namespace sema {

// =============================================================================
// analyze - Main Entry Point
// =============================================================================

// analyze()
//   └── PHASE 1: registerTopLevelNames()
//       └── registerDeclName(outer) → ctx.insertValue(outer)  // ✅ Module table
//
//   └── PHASE 2: resolveModuleDecls()
//       └── resolveFuncDecl(outer)
//           ├── ctx.isAtModuleLevel()? true → no registration needed
//           ├── ctx.pushScope()  // Parameter scope for outer
//           ├── resolveParam(x) → ctx.insertValue(x)  // ✅ Registers x
//           ├── ctx.stack.pushFunction(outer)
//           ├── resolveBlock(outer->body)
//           │   ├── ctx.pushScope()  // Block scope for outer's body
//           │   ├── resolveDeclStmt(inner)
//           │   │   └── resolveDecl(inner) → resolveFuncDecl(inner)
//           │   │       ├── ctx.isAtModuleLevel()? false → ctx.insertValue(inner)  // ✅ Registers inner in outer's block scope
//           │   │       ├── ctx.pushScope()  // Parameter scope for inner
//           │   │       ├── resolveParam(y) → ctx.insertValue(y)  // ✅ Registers y
//           │   │       ├── ctx.stack.pushFunction(inner)  // ✅ Nested context
//           │   │       ├── resolveBlock(inner->body)
//           │   │       │   ├── ctx.pushScope()
//           │   │       │   └── ctx.popScope()
//           │   │       ├── ctx.stack.pop()  // ✅ Pop inner's function context
//           │   │       └── ctx.popScope()  // ✅ Pop inner's parameter scope
//           │   ├── resolveReturnStmt(inner(10))
//           │   │   └── resolveIdentifierExpr(inner)
//           │   │       └── ctx.lookupValue(inner)  // ✅ Finds inner in outer's block scope!
//           │   └── ctx.popScope()  // Clean up outer's block scope
//           ├── ctx.stack.pop()  // Pop outer's function context
//           └── ctx.popScope()  // Clean up outer's parameter scope
void analyze(std::vector<ModuleAST*>& modules, SemaContext& ctx) {
    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 1: Register ALL top-level names (No type resolution)
    // ─────────────────────────────────────────────────────────────────────────
    // 
    // IMPORTANT: Phase 1 ONLY registers top-level declarations.
    // Local variables, parameters, and other scoped names are registered
    // during Phase 2 when we actually resolve the bodies.
    // 
    // This is because:
    // 1. Name resolution needs type information (which we don't have yet)
    // 2. Local scopes are only meaningful during type resolution
    // 3. It's simpler and more correct
    // ─────────────────────────────────────────────────────────────────────────
    for (ModuleAST* module : modules) {
        if (!module) continue;
        ctx.enterModule(module);
        registerTopLevelNames(module, ctx);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // PHASE 2: Resolve ALL types, check bodies, AND evaluate consts
    // ─────────────────────────────────────────────────────────────────────────
    // 
    // During this phase, we resolve types, register local names as we go,
    // perform semantic analysis, and evaluate const expressions.
    // ─────────────────────────────────────────────────────────────────────────

    for (ModuleAST* module : modules) {
        if (!module) continue;

        ctx.enterModule(module);
        resolveModuleDecls(module, ctx);

        module->hasErrors = ctx.diagnostics.hasErrors();

        if (!ctx.diagnostics.canContinue()) {
            return;
        }
    }
}

// =============================================================================
// PHASE 1: Top-Level Name Registration Only
// =============================================================================

/// @brief Register ONLY top-level names in a module (no type resolution).
///
/// This is Phase 1 of semantic analysis. It only registers names that are
/// visible at module scope. Local variables, parameters, and other scoped
/// names are registered during Phase 2 (type resolution).
///
/// @param module The module to register names from.
/// @param ctx The semantic context.
void registerTopLevelNames(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    for (const DeclPtr decl : module->decls) {
        if (!decl) continue;
        
        // Register ONLY top-level declaration names
        registerDeclName(decl, ctx);
        
        // IMPORTANT: We do NOT walk into function bodies here.
        // Local names are registered during Phase 2.
        // 
        // Struct fields are registered by registerStructName
        // (called from registerDeclName)
        
        if (!ctx.diagnostics.canContinue()) {
            return;
        }
    }
}

// ─── registerDeclName ──────────────────────────────────────────────────────

/// @brief Register a declaration's name at the current scope level.
///
/// For top-level declarations, this registers in the module table.
/// For local declarations (called during Phase 2), this registers in the
/// current scope.
///
/// @param decl The declaration to register.
/// @param ctx The semantic context.
void registerDeclName(const DeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl:
            registerImportName(decl->as<ImportDeclAST>(), ctx);
            return;
        case ASTKind::VarDecl:
            registerVarName(decl->as<VarDeclAST>(), ctx);
            return;
        case ASTKind::FuncDecl:
            registerFuncName(decl->as<FuncDeclAST>(), ctx);
            return;
        case ASTKind::EnumDecl:
            registerEnumName(decl->as<EnumDeclAST>(), ctx);
            return;
        case ASTKind::TraitDecl:
            registerTraitName(decl->as<TraitDeclAST>(), ctx);
            return;
        case ASTKind::StructDecl:
            registerStructName(decl->as<StructDeclAST>(), ctx);
            return;
        default:
            return;
    }
}

// =============================================================================
// PHASE 2: Type Resolution
// =============================================================================

/// @brief Resolve all declarations in a module.
///
/// This is the Phase 2 entry point for a module. It walks all top-level
/// declarations and resolves their types, bodies, and const expressions.
///
/// @param module The module to resolve.
/// @param ctx The semantic context.
///
/// @note This function does NOT register names - that was done in Phase 1.
///       However, nested declarations (inside function bodies) will be
///       registered during resolution of those bodies.
void resolveModuleDecls(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    for (const DeclPtr decl : module->decls) {
        if (!decl) continue;
        
        resolveDecl(decl, ctx);
        
        if (!ctx.diagnostics.canContinue()) {
            return;
        }
    }
}

/// @brief Resolve a single declaration.
///
/// Dispatches to the appropriate resolver based on the declaration's kind.
///
/// @param decl The declaration to resolve.
/// @param ctx The semantic context.
///
/// @note This function is also called recursively for nested declarations
///       (e.g., functions defined inside function bodies).
void resolveDecl(const DeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    // Check if we're at module level - if not, we need to register
    // this declaration in the current scope (it's a nested declaration).
    // This handles nested functions, structs, enums, and traits.
    if (!ctx.isAtModuleLevel()) {
        // Register the declaration in the current scope
        switch (decl->kind) {
            case ASTKind::VarDecl:
            case ASTKind::FuncDecl:
                ctx.insertValue(decl->as<ValueDeclAST>());
                break;
            case ASTKind::StructDecl:
            case ASTKind::EnumDecl:
            case ASTKind::TraitDecl:
                ctx.insertType(decl->as<TypeDeclAST>());
                break;
            default:
                // Other declaration kinds don't need registration
                break;
        }
    }

    // Now resolve the declaration's type and body
    switch (decl->kind) {
        case ASTKind::ImportDecl:
            resolveImportDecl(decl->as<ImportDeclAST>(), ctx);
            return;
        case ASTKind::VarDecl:
            resolveVarDecl(decl->as<VarDeclAST>(), ctx);
            return;
        case ASTKind::FuncDecl:
            resolveFuncDecl(decl->as<FuncDeclAST>(), ctx);
            return;
        case ASTKind::EnumDecl:
            resolveEnumDecl(decl->as<EnumDeclAST>(), ctx);
            return;
        case ASTKind::TraitDecl:
            resolveTraitDecl(decl->as<TraitDeclAST>(), ctx);
            return;
        case ASTKind::StructDecl:
            resolveStructDecl(decl->as<StructDeclAST>(), ctx);
            return;
        default:
            // Unknown declaration kind - ignore (error recovery)
            return;
    }
}

// =============================================================================
// Phase 1 Registration Functions (forwarded to SemaDecl.cpp)
// =============================================================================

// These functions are declared in Sema.hpp and implemented in SemaDecl.cpp.
// They are called from registerTopLevelNames() above.

// registerImportName()    - implemented in SemaDecl.cpp
// registerVarName()       - implemented in SemaDecl.cpp
// registerFuncName()      - implemented in SemaDecl.cpp
// registerParamName()     - implemented in SemaDecl.cpp
// registerGenericParamName() - implemented in SemaDecl.cpp
// registerEnumName()      - implemented in SemaDecl.cpp
// registerTraitName()     - implemented in SemaDecl.cpp
// registerStructName()    - implemented in SemaDecl.cpp
// registerStructFieldNames() - implemented in SemaDecl.cpp

// Phase 2 Resolution Functions (forwarded to SemaDecl.cpp, SemaStmt.cpp, SemaExpr.cpp)

// resolveImportDecl()     - implemented in SemaDecl.cpp
// resolveVarDecl()        - implemented in SemaDecl.cpp
// resolveFuncDecl()       - implemented in SemaDecl.cpp
// resolveParam()          - implemented in SemaDecl.cpp
// resolveGenericParam()   - implemented in SemaDecl.cpp
// resolveEnumDecl()       - implemented in SemaDecl.cpp
// resolveTraitDecl()      - implemented in SemaDecl.cpp
// resolveStructDecl()     - implemented in SemaDecl.cpp
// resolveStructFields()   - implemented in SemaDecl.cpp

// resolveStmt()           - implemented in SemaStmt.cpp
// resolveBlock()          - implemented in SemaStmt.cpp
// resolveIfStmt()         - implemented in SemaStmt.cpp
// resolveSwitchStmt()     - implemented in SemaStmt.cpp
// resolveSwitchCase()     - implemented in SemaStmt.cpp
// resolveForStmt()        - implemented in SemaStmt.cpp
// resolveWhileStmt()      - implemented in SemaStmt.cpp
// resolveDoWhileStmt()    - implemented in SemaStmt.cpp
// resolveReturnStmt()     - implemented in SemaStmt.cpp
// resolveBreakStmt()      - implemented in SemaStmt.cpp
// resolveContinueStmt()   - implemented in SemaStmt.cpp
// resolveExprStmt()       - implemented in SemaStmt.cpp
// resolveDeclStmt()       - implemented in SemaStmt.cpp
// resolveAsyncStmt()      - implemented in SemaStmt.cpp
// resolveAwaitStmt()      - implemented in SemaStmt.cpp
// resolveSpawnStmt()      - implemented in SemaStmt.cpp
// resolveJoinStmt()       - implemented in SemaStmt.cpp

// resolveExpr()           - implemented in SemaExpr.cpp
// resolveExprWithTarget() - implemented in SemaExpr.cpp

} // namespace sema