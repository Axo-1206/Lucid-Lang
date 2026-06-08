#include "ast/StmtAST.hpp"
#include "ast/ExprAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/checkers/SemanticChecker.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

void checkExprStmt(ExprStmtAST& node, SemanticContext& ctx) {
    TypeAST* exprType = checkExpr(node.expr.get(), ctx);
    if (exprType && !exprType->isa<PrimitiveTypeAST>()) {
        ctx.warning(node.loc, DiagCode::W6008, "unused result of expression; value discarded");
    }
}