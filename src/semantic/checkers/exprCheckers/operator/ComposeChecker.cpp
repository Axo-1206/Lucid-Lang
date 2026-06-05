/**
 * @file ComposeChecker.cpp
 * @brief Semantic checking for composition expressions (+>).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkComposeExpr(ComposeExprAST& node, SemanticContext& ctx) {
    TypeAST* leftType = checkExpr(node.left.get(), ctx);
    if (!leftType) return nullptr;
    
    if (!leftType->isa<FuncTypeAST>()) {
        ctx.error(node.left->loc, DiagCode::E2002, "left side of composition must be a function");
        return nullptr;
    }
    
    TypeAST* currentType = leftType;
    bool allConst = node.left->isConst;
    
    for (auto& operand : node.operands) {
        if (!operand) continue;
        
        TypeAST* funcType = checkExpr(operand->callable.get(), ctx);
        if (!funcType) return nullptr;
        
        if (!funcType->isa<FuncTypeAST>()) {
            ctx.error(operand->loc, DiagCode::E2002, "compose operand is not a function");
            return nullptr;
        }
        
        FuncTypeAST* curFunc = currentType->as<FuncTypeAST>();
        FuncTypeAST* nextFunc = funcType->as<FuncTypeAST>();
        
        // Check return type of left matches first parameter of right
        if (curFunc->sig.returnTypes.empty()) {
            ctx.error(operand->loc, DiagCode::E2002, "left function has no return type");
            return nullptr;
        }
        
        if (nextFunc->sig.groupCount() == 0) {
            ctx.error(operand->loc, DiagCode::E2002, "right function has no parameter groups");
            return nullptr;
        }
        
        auto nextFirstGroup = nextFunc->sig.getGroup(0);
        if (nextFirstGroup.empty()) {
            ctx.error(operand->loc, DiagCode::E2002, "right function has no parameters");
            return nullptr;
        }
        
        TypeAST* curRet = curFunc->sig.returnTypes[0].get();
        TypeAST* nextFirstParam = nextFirstGroup[0]->type.get();
        
        if (!TypeChecker::isEqual(curRet, nextFirstParam, ctx)) {
            ctx.error(operand->loc, DiagCode::E2002,
                      "composition type mismatch: output of left (",
                      LucDebug::formatType(curRet, ctx.pool), ") does not match input of right (",
                      LucDebug::formatType(nextFirstParam, ctx.pool), ")");
            return nullptr;
        }
        
        currentType = funcType;
        allConst = allConst && operand->callable->isConst;
    }
    
    node.isConst = allConst;
    node.resolvedType = currentType;
    return currentType;
}