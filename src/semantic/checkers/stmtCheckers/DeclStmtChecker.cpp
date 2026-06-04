#include "ast/StmtAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "debug/DebugUtils.hpp"

void checkDeclStmt(DeclStmtAST& node, SemanticContext& ctx) {
    if (!node.decl) return;

    if (auto* varDecl = node.decl->as<VarDeclAST>()) {
        checkVarDecl(*varDecl, ctx, true);
    } else if (auto* funcDecl = node.decl->as<FuncDeclAST>()) {
        checkFuncDecl(*funcDecl, ctx, true);
    } else {
        checkTopLevelDecl(node.decl.get(), ctx);
    }
}