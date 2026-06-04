#include "ast/StmtAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "debug/DebugUtils.hpp"

void checkBlockStmt(BlockStmtAST& node, SemanticContext& ctx, TypeAST* expectedReturn) {
    ctx.symbols->pushScope();
    for (auto& stmt : node.stmts) {
        checkStmt(stmt.get(), ctx, expectedReturn);
    }
    ctx.symbols->popScope();
}