/**
 * @file RangeChecker.cpp
 * @brief Semantic checking for range expressions (lo..hi, lo..<hi).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkRangeExpr(RangeExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkRangeExpr: exclusive=" << node.isExclusive);
    
    // Check lower bound
    TypeAST* loType = checkExpr(node.lo.get(), ctx);
    if (!loType) return nullptr;
    
    // Check upper bound
    TypeAST* hiType = checkExpr(node.hi.get(), ctx);
    if (!hiType) return nullptr;
    
    // Both bounds must be integers
    if (!TypeChecker::isIntegerType(loType, ctx) || !TypeChecker::isIntegerType(hiType, ctx)) {
        ctx.error(node.loc, DiagCode::E2002, "range bounds must be integers");
        return nullptr;
    }
    
    // Check if bounds are compatible
    if (!TypeChecker::isAssignable(loType, hiType, ctx) &&
        !TypeChecker::isAssignable(hiType, loType, ctx)) {
        ctx.error(node.loc, DiagCode::E2002, "range bounds have incompatible types");
        return nullptr;
    }
    
    // Validate constant bounds if both are constant
    int64_t loVal, hiVal;
    bool loConst = TypeChecker::getConstantIntValue(node.lo.get(), loVal, ctx);
    bool hiConst = TypeChecker::getConstantIntValue(node.hi.get(), hiVal, ctx);
    
    if (loConst && hiConst) {
        if (node.isExclusive) {
            if (loVal >= hiVal) {
                ctx.warning(node.loc, DiagCode::W6001,
                            "exclusive range '", loVal, "..<", hiVal, "' is empty");
            }
        } else {
            if (loVal > hiVal) {
                ctx.warning(node.loc, DiagCode::W6001,
                            "inclusive range '", loVal, "..", hiVal, "' is empty");
            }
        }
    }
    
    // Range expression itself has no inherent type; it's used in for loops or match patterns
    // For now, return a placeholder type (any) - the actual type will be determined by context
    node.isConst = loConst && hiConst;
    node.resolvedType = ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Any).release();
    return node.resolvedType;
}