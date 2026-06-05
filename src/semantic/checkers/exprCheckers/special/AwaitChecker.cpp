/**
 * @file AwaitChecker.cpp
 * @brief Semantic checking for await expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkAwaitExpr(AwaitExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkAwaitExpr");
    
    // Check inner expression
    TypeAST* innerType = checkExpr(node.inner.get(), ctx);
    if (!innerType) return nullptr;
    
    // Validate that we're inside an async function
    // Note: This requires tracking async context. For now, we assume the caller
    // (function checker) sets a flag. We'll check a depth counter or flag.
    // Since SemanticContext doesn't have asyncDepth yet, we add a check that
    // can be expanded later.
    
    // TODO: Add ctx.asyncDepth tracking when implementing async function checking
    // For now, we'll emit a warning if not obviously in async context
    // The actual validation will be done by the function checker
    
    // The awaited expression must be a call to an async function
    if (!node.inner->isa<CallExprAST>()) {
        ctx.error(node.loc, DiagCode::E2024, "'await' can only be used on function calls");
        return nullptr;
    }
    
    auto* call = node.inner->as<CallExprAST>();
    if (!call->isAsyncCall) {
        ctx.error(node.loc, DiagCode::E2024, "'await' requires an ~async function call");
        return nullptr;
    }
    
    // await expression returns the same type as the awaited call
    node.resolvedType = innerType;
    node.isConst = false; // await result is never const (runtime-dependent)
    return innerType;
}