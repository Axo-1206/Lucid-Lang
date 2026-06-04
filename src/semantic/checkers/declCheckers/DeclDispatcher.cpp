/**
 * @file DeclDispatcher.cpp
 *
 * @nutshell Verifies the structure of structs, enums, functions, and implementations.
 *
 * @responsibility Phase 3a of semantic analysis: walks declaration nodes, resolves their
 *   types via TypeResolver, and enforces all declaration-level rules.
 *
 * @related SemanticAnalyzer.cpp, SemanticStmt.cpp, SemanticExpr.cpp
 */

#include "ast/ExprAST.hpp"
#include "ast/support/ArenaSpan.hpp"
#include "ast/support/StringPool.hpp"
#include "debug/DebugUtils.hpp"
#include "registry/AttributeRegistry.hpp"
#include "semantic/SymbolTable.hpp"
#include "semantic/resolveType/TypeResolver.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"

#include <unordered_set>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations of other checking functions (defined in other .cpp files)
// ─────────────────────────────────────────────────────────────────────────────
void checkVarDecl(VarDeclAST& node, SemanticContext& ctx, bool isLocal);
void checkFuncDecl(FuncDeclAST& node, SemanticContext& ctx, bool isLocal);
void checkStructDecl(StructDeclAST& node, SemanticContext& ctx, bool isLocal);
void checkEnumDecl(EnumDeclAST& node, SemanticContext& ctx, bool isLocal);
void checkTraitDecl(TraitDeclAST& node, SemanticContext& ctx, bool isLocal);
void checkImplDecl(ImplDeclAST& node, SemanticContext& ctx, bool isLocal);
void checkFromDecl(FromDeclAST& node, SemanticContext& ctx, bool isLocal);

// ─────────────────────────────────────────────────────────────────────────────
// checkTopLevelDecl  — Dispatcher called by SemanticAnalyzer::checkDecls()
// ─────────────────────────────────────────────────────────────────────────────
void checkTopLevelDecl(DeclAST* decl, SemanticContext& ctx) {
    if (!decl) return;
    LUC_LOG_SEMANTIC("checkTopLevelDecl: kind=" << LucDebug::kindToString(decl->kind));

    if (decl->isa<VarDeclAST>())
        checkVarDecl(*decl->as<VarDeclAST>(), ctx, false);
    else if (decl->isa<FuncDeclAST>())
        checkFuncDecl(*decl->as<FuncDeclAST>(), ctx, false);
    else if (decl->isa<StructDeclAST>())
        checkStructDecl(*decl->as<StructDeclAST>(), ctx, false);
    else if (decl->isa<EnumDeclAST>())
        checkEnumDecl(*decl->as<EnumDeclAST>(), ctx, false);
    else if (decl->isa<TraitDeclAST>())
        checkTraitDecl(*decl->as<TraitDeclAST>(), ctx, false);
    else if (decl->isa<ImplDeclAST>())
        checkImplDecl(*decl->as<ImplDeclAST>(), ctx, false);
    else if (decl->isa<FromDeclAST>())
        checkFromDecl(*decl->as<FromDeclAST>(), ctx, false);
}