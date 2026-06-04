/**
 * @file NullCoalesceChecker.cpp
 * @brief Semantic checking for null coalescing expressions (??).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkNullCoalesceExpr(NullCoalesceExprAST& node, SemanticContext& ctx) {
    TypeAST* leftType = checkExpr(node.value.get(), ctx);
    if (!leftType) return nullptr;
    
    TypeAST* rightType = checkExpr(node.fallback.get(), ctx);
    if (!rightType) return nullptr;
    
    // Left side must be nullable
    if (!TypeChecker::isNullable(leftType, ctx)) {
        ctx.error(node.loc, DiagCode::E2002, "left side of '\?\?' must be nullable");
        return nullptr;
    }
    
    // Unwrap nullable to get inner type
    TypeAST* innerLeft = leftType->isa<NullableTypeAST>()
                         ? leftType->as<NullableTypeAST>()->inner.get()
                         : leftType;
    
    // Check compatibility between inner left and right
    if (!TypeChecker::isAssignable(innerLeft, rightType, ctx) &&
        !TypeChecker::isAssignable(rightType, innerLeft, ctx)) {
        ctx.error(node.loc, DiagCode::E2002, "fallback type incompatible with nullable inner type");
        return nullptr;
    }
    
    // Result is the unified type
    TypeAST* result = TypeChecker::unify(innerLeft, rightType, ctx);
    if (!result) result = rightType;
    
    node.isConst = node.value->isConst && node.fallback->isConst;
    return result;
}