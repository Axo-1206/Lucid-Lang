/**
 * @file IsChecker.cpp
 * @brief Semantic checking for type check expressions (x is Type).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkIsExpr(IsExprAST& node, SemanticContext& ctx) {
    TypeAST* exprType = checkExpr(node.expr.get(), ctx);
    if (!exprType) return nullptr;
    
    TypeAST* checkType = nullptr;
    if (ctx.dispatcher) {
        checkType = ctx.dispatcher->resolveType(node.checkType.get());
    }
    if (!checkType) {
        ctx.error(node.loc, DiagCode::E2001, "cannot resolve type in 'is' expression");
        return nullptr;
    }
    
    // is expression always returns bool
    node.isConst = node.expr->isConst; // const if the expression is const
    return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool).release();
}