/**
 * @file AnonFuncChecker.cpp
 * @brief Semantic checking for anonymous function expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/StmtAST.hpp"
#include "semantic/resolveType/TypeDispatcher.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkAnonFuncExpr(AnonFuncExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkAnonFuncExpr: params=" << node.sig.totalParamCount()
                             << ", returns=" << node.sig.returnTypes.size());

    // Create a FuncTypeAST from the anonymous function's signature
    auto* funcType = ctx.arena.make<FuncTypeAST>().release();
    funcType->loc = node.loc;
    // Manually copy signature fields (copy assignment is deleted)
    funcType->sig.allParams = node.sig.allParams;
    funcType->sig.groupSizes = node.sig.groupSizes;
    funcType->sig.returnTypes = node.sig.returnTypes;
    funcType->qualifiers = 0; // anonymous functions have no qualifiers

    // Resolve the function type (this resolves parameter and return types)
    TypeAST* resolvedType = ctx.dispatcher ? ctx.dispatcher->resolveType(funcType) : nullptr;
    if (!resolvedType) {
        ctx.error(node.loc, DiagCode::E2002, "invalid anonymous function signature");
        return nullptr;
    }

    // Push scope for parameters
    ctx.symbols->pushScope();

    // Declare parameters (using the original signature, which is the same as funcType->sig)
    for (const auto& param : node.sig.allParams) {
        if (!param) continue;

        // Resolve parameter type if needed
        TypeAST* paramType = param->type.get();
        if (!paramType && ctx.dispatcher) {
            paramType = ctx.dispatcher->resolveType(param->type.get());
            if (!paramType) {
                ctx.error(param->loc, DiagCode::E2001,
                          "cannot resolve type for parameter '", ctx.pool.lookup(param->name), "'");
                continue;
            }
        }

        Symbol paramSym;
        paramSym.name = param->name;
        paramSym.kind = SymbolKind::Param;
        paramSym.declKw = DeclKeyword::Let;
        paramSym.visibility = Visibility::Private;
        paramSym.type = paramType;
        paramSym.decl = param.get();
        paramSym.loc = param->loc;

        if (!ctx.symbols->declare(paramSym)) {
            ctx.error(param->loc, DiagCode::E2005,
                      "duplicate parameter name '", ctx.pool.lookup(param->name), "'");
        }
    }

    // Determine expected return type for body checking
    TypeAST* expectedReturn = nullptr;
    if (!node.sig.returnTypes.empty()) {
        expectedReturn = node.sig.returnTypes[0].get();
        // Ensure the return type is resolved
        if (ctx.dispatcher && expectedReturn) {
            expectedReturn = ctx.dispatcher->resolveType(expectedReturn);
        }
    }

    // Check function body
    if (node.body) {
        checkStmt(node.body.get(), ctx, expectedReturn);
    } else {
        ctx.error(node.loc, DiagCode::E2003, "anonymous function must have a body");
    }

    ctx.symbols->popScope();

    node.resolvedType = funcType;
    return funcType;
}