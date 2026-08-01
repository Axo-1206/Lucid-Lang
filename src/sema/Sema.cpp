/// @file Sema.cpp
/// @brief Implements the public API for the semantic phase.

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

        // Use the new diagnostic system via SemaContext
        module->hasErrors = ctx.diagnostics.hasErrors();

        if (!ctx.canContinue()) {
            return;
        }
    }
    
    // Note: Const evaluation happens during resolveVarDecl, not as a separate phase.
    // The ConstEvaluator instance is used by resolveVarDecl when it sees const declarations.
}

// =============================================================================
// PHASE 1: Name Registration - Module Level
// =============================================================================

/// @brief Register all names in a module (no type resolution).
///
/// This is the Phase 1 entry point for a module. It walks all top-level
/// declarations and registers their names in the symbol table.
///
/// REGISTRATION FLOW:
///   1. Enter the module's scope (ctx.enterModule already called)
///   2. For each declaration, call registerDeclName()
///   3. For statements inside declarations, call registerStmtNames()
///
/// @param module The module to register names from.
/// @param ctx The semantic context.
///
/// @note This function does NOT resolve types - it only registers names.
///       Type resolution happens in Phase 2 (resolveModuleDecls).
void registerModuleNames(ModuleAST* module, SemaContext& ctx) {
    if (!module) return;

    // ─── Register top-level declarations ──────────────────────────────────
    for (const DeclPtr decl : module->decls) {
        if (!decl) continue;
        
        // Register the declaration's name
        registerDeclName(decl, ctx);
        
        // ─── Register names inside statements (function bodies, etc.) ────
        // If this is a function declaration, register names in its body
        if (decl->isa<FuncDeclAST>()) {
            const FuncDeclAST* func = decl->as<FuncDeclAST>();
            if (func->body) {
                // Register names in the function body
                registerStmtNames(func->body, ctx);
            }
        }
        
        // If this is a struct declaration, fields are already registered
        // by registerStructName (which calls registerStructFieldNames)
        
        // If this is a trait declaration, fields are requirements, not values
        // No registration needed for trait fields

        if (!ctx.canContinue()) {
            return;
        }
    }
}

// ─── registerDeclName ──────────────────────────────────────────────────────

/// @brief Register a declaration's name only (no type resolution).
///
/// Dispatches to the appropriate registration function based on the
/// declaration's kind.
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
            // Unknown declaration kind - ignore (error recovery)
            return;
    }
}

// ─── registerStmtNames ────────────────────────────────────────────────────

/// @brief Register names in a statement (for local scopes).
///
/// Walks a statement AST and registers all names introduced by declarations
/// inside it. This is used for function bodies, blocks, and other scoped
/// constructs.
///
/// @param stmt The statement to walk.
/// @param ctx The semantic context.
///
/// @note This function is called during Phase 1 (name registration).
///       It does NOT resolve types or validate semantics.
void registerStmtNames(const StmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return;

    switch (stmt->kind) {
        case ASTKind::BlockStmt: {
            const BlockStmtAST* block = stmt->as<BlockStmtAST>();
            // Push a new scope for the block
            ctx.pushScope();
            for (const StmtPtr s : block->stmts) {
                registerStmtNames(s, ctx);
            }
            ctx.popScope();
            return;
        }

        case ASTKind::DeclStmt: {
            const DeclStmtAST* declStmt = stmt->as<DeclStmtAST>();
            // Register the declaration's name
            registerDeclName(declStmt->decl, ctx);
            return;
        }

        case ASTKind::ForStmt: {
            const ForStmtAST* forStmt = stmt->as<ForStmtAST>();
            // For loop variables are in their own scope
            ctx.pushScope();
            
            // Register index variable
            if (forStmt->indexVar) {
                registerParamName(forStmt->indexVar, ctx);
            }
            
            // Register value variable
            if (forStmt->valueVar) {
                registerParamName(forStmt->valueVar, ctx);
            }
            
            // Register names in the loop body
            if (forStmt->body) {
                registerStmtNames(forStmt->body, ctx);
            }
            
            ctx.popScope();
            return;
        }

        case ASTKind::IfStmt: {
            const IfStmtAST* ifStmt = stmt->as<IfStmtAST>();
            // If condition doesn't introduce names
            // Register names in branches
            if (ifStmt->thenBranch) {
                registerStmtNames(ifStmt->thenBranch, ctx);
            }
            if (ifStmt->elseBranch) {
                registerStmtNames(ifStmt->elseBranch, ctx);
            }
            return;
        }

        case ASTKind::WhileStmt: {
            const WhileStmtAST* whileStmt = stmt->as<WhileStmtAST>();
            if (whileStmt->body) {
                registerStmtNames(whileStmt->body, ctx);
            }
            return;
        }

        case ASTKind::SwitchStmt: {
            const SwitchStmtAST* switchStmt = stmt->as<SwitchStmtAST>();
            // Switch cases don't introduce names (they use existing names)
            // But register names in case bodies
            for (const SwitchCasePtr caseStmt : switchStmt->cases) {
                if (caseStmt->body) {
                    registerStmtNames(caseStmt->body, ctx);
                }
            }
            if (switchStmt->defaultBody) {
                registerStmtNames(switchStmt->defaultBody, ctx);
            }
            return;
        }

        default:
            // Other statements don't introduce names
            // (ReturnStmt, BreakStmt, ContinueStmt, ExprStmt, etc.)
            return;
    }
}

} // namespace sema