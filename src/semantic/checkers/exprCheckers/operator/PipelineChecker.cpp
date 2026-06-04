/**
 * @file PipelineChecker.cpp
 * @brief Semantic checking for pipeline expressions (|>).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkPipelineExpr(PipelineExprAST& node, SemanticContext& ctx) {
    TypeAST* currentType = checkExpr(node.seed.get(), ctx);
    if (!currentType) return nullptr;
    
    bool allConst = node.seed->isConst;
    
    for (auto& step : node.steps) {
        if (!step) continue;
        
        // Evaluate the step's callable
        TypeAST* stepType = checkExpr(step->callable.get(), ctx);
        if (!stepType) return nullptr;
        
        // Step must be a function type
        if (!stepType->isa<FuncTypeAST>()) {
            ctx.error(step->loc, DiagCode::E2002, "pipeline step is not a function");
            return nullptr;
        }
        
        FuncTypeAST* func = stepType->as<FuncTypeAST>();
        
        // Check if function is callable with current type as first argument
        if (func->sig.groupCount() == 0) {
            ctx.error(step->loc, DiagCode::E2002, "pipeline step function has no parameter groups");
            return nullptr;
        }
        
        auto firstGroup = func->sig.getGroup(0);
        if (firstGroup.empty()) {
            ctx.error(step->loc, DiagCode::E2002, "pipeline step function has no parameters");
            return nullptr;
        }
        
        TypeAST* firstParamType = firstGroup[0]->type.get();
        if (!TypeChecker::isAssignable(currentType, firstParamType, ctx)) {
            ctx.error(step->loc, DiagCode::E2002,
                      "pipeline seed/result type does not match step's first parameter");
            return nullptr;
        }
        
        // Check argument pack if present
        if (!step->packArgs.empty()) {
            // Additional arguments after the first parameter
            size_t expectedExtra = firstGroup.size() - 1;
            if (step->packArgs.size() != expectedExtra) {
                ctx.error(step->loc, DiagCode::E2003,
                          "argument pack count mismatch: expected ", expectedExtra,
                          ", got ", step->packArgs.size());
                return nullptr;
            }
            
            for (size_t i = 0; i < step->packArgs.size(); ++i) {
                TypeAST* argType = checkExpr(step->packArgs[i].get(), ctx);
                if (!argType) return nullptr;
                
                TypeAST* paramType = firstGroup[i + 1]->type.get();
                if (!TypeChecker::isAssignable(argType, paramType, ctx)) {
                    ctx.error(step->packArgs[i]->loc, DiagCode::E2002,
                              "argument ", i + 1, " type mismatch in pipeline step");
                    return nullptr;
                }
            }
        } else if (firstGroup.size() != 1) {
            ctx.error(step->loc, DiagCode::E2003,
                      "function expects ", firstGroup.size(),
                      " arguments, but no argument pack provided (use '!')");
            return nullptr;
        }
        
        // Update current type to function's return type
        if (func->sig.returnTypes.empty()) {
            currentType = nullptr;
        } else {
            currentType = func->sig.returnTypes[0].get();
        }
        
        allConst = allConst && step->callable->isConst;
    }
    
    node.isConst = allConst;
    node.resolvedType = currentType;
    return currentType;
}