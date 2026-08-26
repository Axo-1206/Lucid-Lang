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

    for (DeclAST* decl : module->decls) {
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
void registerDeclName(DeclAST* decl, SemaContext& ctx) {
    if (!decl || decl->name.isEmpty()) return;

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

    for (DeclAST* decl : module->decls) {
        if (!decl) continue;
        
        resolveDecl(decl, ctx);
        
        if (!ctx.diagnostics.canContinue()) {
            return;
        }
    }
}

// ─── Resolution Entry Point ────────────────────────────────────────────────

/// @brief Resolve a single declaration.
///
/// This is the main entry point for declaration resolution.
/// It handles:
///   1. Registering nested declarations (Phase 2 registration)
///   2. Dispatching to the appropriate resolver
///
/// @note Top-level declarations are already registered in Phase 1.
///       Only nested declarations are registered here.
void resolveDecl(DeclAST* decl, SemaContext& ctx) {
    if (!decl) return;

    // ─── PHASE 2 REGISTRATION: Nested declarations only ──────────────────
    // Top-level declarations are already registered in Phase 1.
    // Nested declarations (inside functions, blocks, etc.) are registered
    // when the resolver encounters them during Phase 2.
    if (!ctx.isAtModuleLevel()) {
        if (!decl->name.isEmpty()) {
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
    }

    if (decl->hasSyntaxError) {
        if (decl->kind == ASTKind::VarDecl) {
            decl->as<VarDeclAST>()->type = ctx.getUnknownType();
        }
        return;
    }

    // ─── DISPATCH TO RESOLVER ─────────────────────────────────────────────
    // The resolver functions below do NOT register the declaration again.
    // They only resolve types, check bodies, and evaluate consts.
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

} // namespace sema