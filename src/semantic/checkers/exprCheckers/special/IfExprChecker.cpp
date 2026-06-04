/**
 * @file IfExprChecker.cpp
 * @brief Semantic checking for if expressions (ternary-like).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkIfExpr(IfExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkIfExpr");
    
    // Check condition (must be boolean)
    TypeAST* condType = checkExpr(node.condition.get(), ctx);
    if (!condType) return nullptr;
    
    if (!TypeChecker::isBooleanCompatible(condType, ctx)) {
        ctx.error(node.condition->loc, DiagCode::E2002, "if condition must be boolean");
        return nullptr;
    }
    
    // Check then branch
    TypeAST* thenType = checkExpr(node.thenBranch.get(), ctx);
    if (!thenType) return nullptr;
    
    // Check else branch
    TypeAST* elseType = checkExpr(node.elseBranch.get(), ctx);
    if (!elseType) return nullptr;
    
    // Unify then and else types
    TypeAST* unified = TypeChecker::unify(thenType, elseType, ctx);
    if (!unified) {
        ctx.error(node.loc, DiagCode::E2002,
                  "then and else branches must have compatible types (got '",
                  LucDebug::formatType(thenType, ctx.pool), "' and '",
                  LucDebug::formatType(elseType, ctx.pool), "')");
        return nullptr;
    }
    
    node.isConst = node.condition->isConst && node.thenBranch->isConst && node.elseBranch->isConst;
    node.resolvedType = unified;
    return unified;
}