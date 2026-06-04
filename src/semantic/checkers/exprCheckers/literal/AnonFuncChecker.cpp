/**
 * @file AnonFuncChecker.cpp
 * @brief Semantic checking for anonymous function expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "ast/StmtAST.hpp"
#include "semantic/resolveType/TypeResolver.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkAnonFuncExpr(AnonFuncExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkAnonFuncExpr: params=" << node.sig.totalParamCount()
                             << ", returns=" << node.sig.returnTypes.size());
    
    // Clone and resolve the function type
    FuncTypeAST* funcType = nullptr;
    if (ctx.resolver) {
        funcType = ctx.resolver->cloneFuncSignature(node.sig, node.loc);
        TypeAST* resolvedType = ctx.resolver->resolveType(funcType);
        if (!resolvedType) {
            ctx.error(node.loc, DiagCode::E2002, "invalid anonymous function signature");
            return nullptr;
        }
    } else {
        ctx.error(node.loc, DiagCode::E2002, "type resolver not available");
        return nullptr;
    }
    
    // Push scope for parameters
    ctx.symbols->pushScope();
    
    // Declare parameters
    for (const auto& param : node.sig.allParams) {
        if (!param) continue;
        
        // Resolve parameter type if needed
        TypeAST* paramType = param->type.get();
        if (!paramType && ctx.resolver) {
            paramType = ctx.resolver->resolveType(param->type.get());
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
    if (!funcType->sig.returnTypes.empty()) {
        expectedReturn = funcType->sig.returnTypes[0].get();
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