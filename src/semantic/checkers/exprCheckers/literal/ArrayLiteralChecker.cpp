/**
 * @file ArrayLiteralChecker.cpp
 * @brief Semantic checking for array literal expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/checkType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkArrayLiteralExpr(ArrayLiteralExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkArrayLiteralExpr: elements=" << node.elements.size());
    
    if (node.elements.empty()) {
        // Empty array literal – type will be inferred from context
        // For now, return nullptr (type to be determined by parent)
        node.resolvedType = nullptr;
        return nullptr;
    }
    
    // Check first element to establish base type
    TypeAST* firstType = checkExpr(node.elements[0].get(), ctx);
    if (!firstType) return nullptr;
    
    TypeAST* unifiedType = firstType;
    
    // Check remaining elements for type consistency
    for (size_t i = 1; i < node.elements.size(); ++i) {
        TypeAST* elemType = checkExpr(node.elements[i].get(), ctx);
        if (!elemType) return nullptr;
        
        unifiedType = TypeChecker::unify(unifiedType, elemType, ctx);
        if (!unifiedType) {
            ctx.error(node.elements[i]->loc, DiagCode::E2002,
                      "array element ", i, " has incompatible type with previous elements");
            return nullptr;
        }
    }
    
    // Check if any element is const (all elements must be const for const array)
    bool allConst = true;
    for (const auto& elem : node.elements) {
        if (elem && !elem->isConst) {
            allConst = false;
            break;
        }
    }
    node.isConst = allConst;
    
    node.resolvedType = unifiedType;
    return unifiedType;
}