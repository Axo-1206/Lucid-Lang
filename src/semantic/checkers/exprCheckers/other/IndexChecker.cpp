/**
 * @file IndexChecker.cpp
 * @brief Semantic checking for array/slice index expressions.
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkIndexExpr(IndexExprAST& node, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkIndexExpr: kind=" << static_cast<int>(node.kind));
    
    TypeAST* targetType = checkExpr(node.target.get(), ctx);
    if (!targetType) return nullptr;
    
    // Check if target is nullable
    if (TypeChecker::isNullable(targetType, ctx)) {
        ctx.error(node.loc, DiagCode::E2002, "cannot index nullable value; use ?. or guard first");
        return nullptr;
    }
    
    // Determine element type based on target type
    TypeAST* elementType = nullptr;
    
    if (targetType->isa<ArrayTypeAST>()) {
        auto* arr = targetType->as<ArrayTypeAST>();
        elementType = arr->element.get();
    } else if (targetType->isa<PtrTypeAST>()) {
        // Raw pointers cannot be indexed directly
        ctx.error(node.loc, DiagCode::E2002, "cannot index raw pointer (*T); use #ptrOffset intrinsic");
        return nullptr;
    } else {
        ctx.error(node.loc, DiagCode::E2002, "index operator only allowed on array types");
        return nullptr;
    }
    
    if (node.kind == IndexKind::Element) {
        // Element access: arr[index]
        if (!TypeChecker::isValidArrayIndex(node.index.get(), node.loc, ctx)) {
            return nullptr;
        }
        
        node.resolvedType = elementType;
        node.isConst = node.target->isConst && node.index->isConst;
        return elementType;
        
    } else if (node.kind == IndexKind::Slice) {
        // Slice access: arr[start..end] or arr[start..<end]
        if (!TypeChecker::isValidSliceBound(node.index.get(), "start", node.loc, ctx) ||
            !TypeChecker::isValidSliceBound(node.sliceEnd.get(), "end", node.loc, ctx)) {
            return nullptr;
        }
        
        // Check that start <= end (only for constant bounds)
        int64_t startVal, endVal;
        bool startConst = TypeChecker::getConstantIntValue(node.index.get(), startVal, ctx);
        bool endConst = TypeChecker::getConstantIntValue(node.sliceEnd.get(), endVal, ctx);
        
        if (startConst && endConst && startVal > endVal) {
            ctx.error(node.loc, DiagCode::E2002,
                      "slice start (", startVal, ") must be <= end (", endVal, ")");
            return nullptr;
        }
        
        // Slice operation returns a slice type
        if (!node.sliceType) {
            node.sliceType = ctx.arena.make<ArrayTypeAST>(ArrayKind::Slice, 0, TypePtr(elementType));
        }
        
        TypeAST* sliceRes = node.sliceType.get();
        node.resolvedType = sliceRes;
        node.isConst = false; // Slice result is never const (views runtime data)
        return sliceRes;
    }
    
    return nullptr;
}