/**
 * @file CallChecker.cpp
 * @brief Semantic checking for function call expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "semantic/checkers/declCheckers/DeclHelpers.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkCallExpr(CallExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkCallExpr: args=" << node.args.size());
    
    // Check callee expression
    TypeAST* calleeType = checkExpr(node.callee.get(), ctx);
    if (!calleeType) return nullptr;
    
    // Check if callee is callable
    if (!TypeChecker::isCallable(calleeType, ctx)) {
        ctx.error(node.loc, DiagCode::E2002, "callee is not a function");
        return nullptr;
    }
    
    // Extract function type (handling nullable wrapper)
    FuncTypeAST* funcType = nullptr;
    bool isNullableCall = false;
    
    if (calleeType->isa<FuncTypeAST>()) {
        funcType = calleeType->as<FuncTypeAST>();
    } else if (calleeType->isa<NullableTypeAST>()) {
        auto* nullable = calleeType->as<NullableTypeAST>();
        if (nullable->inner->isa<FuncTypeAST>()) {
            funcType = nullable->inner->as<FuncTypeAST>();
            isNullableCall = true;
            ctx.warning(node.loc, DiagCode::W6014, "calling nullable function; will panic if nil");
        } else {
            ctx.error(node.loc, DiagCode::E2002, "nullable value is not a function");
            return nullptr;
        }
    } else {
        ctx.error(node.loc, DiagCode::E2002, "callee is not a function type");
        return nullptr;
    }
    
    // Get first parameter group
    if (funcType->sig.groupCount() == 0) {
        ctx.error(node.loc, DiagCode::E2002, "function has no parameter groups");
        return nullptr;
    }
    
    auto firstGroup = funcType->sig.getGroup(0);
    
    // Check argument count
    if (firstGroup.size() != node.args.size()) {
        ctx.error(node.loc, DiagCode::E2003,
                  "argument count mismatch: expected ", firstGroup.size(),
                  ", got ", node.args.size());
        return nullptr;
    }
    
    // Check each argument type against parameter type
    for (size_t i = 0; i < firstGroup.size(); ++i) {
        TypeAST* argType = checkExpr(node.args[i].get(), ctx);
        if (!argType) return nullptr;
        
        TypeAST* paramType = firstGroup[i]->type.get();
        if (!paramType) {
            ctx.error(node.loc, DiagCode::E2001, "parameter type not resolved");
            return nullptr;
        }
        
        if (!TypeChecker::isAssignable(argType, paramType, ctx)) {
            // Try to find a `from` conversion
            Symbol* fromCast = TypeChecker::isFromCastable(argType, paramType, ctx);
            if (fromCast && fromCast->decl && fromCast->decl->isa<FromEntryAST>()) {
                // Conversion exists – treat as assignable (no AST rewrite needed)
                // The actual conversion will be handled during code generation
                continue;
            } else {
                ctx.error(node.args[i]->loc, DiagCode::E2002,
                          "argument ", i + 1, " type mismatch: expected ",
                          LucDebug::formatType(paramType, ctx.pool), ", got ",
                          LucDebug::formatType(argType, ctx.pool));
                return nullptr;
            }
        }
    }
    
    // Check async call requirement
    if (funcType->isAsync()) {
        node.isAsyncCall = true;
        // The caller (await expression) will validate async context
    }
    
    // Determine return type
    if (funcType->sig.returnTypes.empty()) {
        node.resolvedType = nullptr;
        return nullptr;
    }
    
    TypeAST* retType = funcType->sig.returnTypes[0].get();
    node.resolvedType = retType;
    return retType;
}