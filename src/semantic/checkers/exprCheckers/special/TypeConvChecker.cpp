/**
 * @file TypeConvChecker.cpp
 * @brief Semantic checking for explicit type conversions (T(expr)).
 */

#include "semantic/checkers/SemanticChecker.hpp"
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"
#include "semantic/resolveType/TypeChecker.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "debug/DebugUtils.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

TypeAST* checkTypeConvExpr(TypeConvExprAST& node, SemanticContext& ctx) {
    
    // Check source expression
    TypeAST* srcType = checkExpr(node.expr.get(), ctx);
    if (!srcType) return nullptr;
    
    // Resolve target type
    TypeAST* targetType = nullptr;
    if (ctx.resolver) {
        targetType = ctx.resolver->resolveType(node.targetType.get());
    }
    if (!targetType) {
        ctx.error(node.loc, DiagCode::E2001, "cannot resolve target type for conversion");
        return nullptr;
    }
    
    // Safe conversion cases
    
    // 1. Primitive to primitive (widening or narrowing with explicit cast)
    if (srcType->isa<PrimitiveTypeAST>() && targetType->isa<PrimitiveTypeAST>()) {
        // Explicit cast allowed between any numeric types
        // The programmer takes responsibility for overflow
        node.resolvedType = targetType;
        node.isConst = node.expr->isConst;
        return targetType;
    }
    
    // 2. Enum to underlying integer type
    if (srcType->isa<NamedTypeAST>() && targetType->isa<PrimitiveTypeAST>()) {
        Symbol* sym = ctx.symbols->lookup(srcType->as<NamedTypeAST>()->name);
        if (sym && sym->kind == SymbolKind::Enum) {
            node.resolvedType = targetType;
            node.isConst = node.expr->isConst;
            return targetType;
        }
    }
    
    // 3. From conversion (if a 'from' block exists)
    Symbol* fromEntry = TypeChecker::isFromCastable(srcType, targetType, ctx);
    if (fromEntry) {
        node.resolvedType = targetType;
        node.isConst = false; // From conversions may have runtime logic
        return targetType;
    }
    
    // 4. Reference to pointer (safe)
    if (srcType->isa<RefTypeAST>() && targetType->isa<PtrTypeAST>()) {
        node.resolvedType = targetType;
        node.isConst = false;
        return targetType;
    }
    
    // 5. Pointer to reference (safe, but requires validity)
    if (srcType->isa<PtrTypeAST>() && targetType->isa<RefTypeAST>()) {
        if (!ctx.insideExtern) {
            ctx.warning(node.loc, DiagCode::W6008,
                        "converting raw pointer to reference; ensure pointer is valid");
        }
        node.resolvedType = targetType;
        node.isConst = false;
        return targetType;
    }
    
    ctx.error(node.loc, DiagCode::E2008,
              "cannot convert from '", LucDebug::formatType(srcType, ctx.pool),
              "' to '", LucDebug::formatType(targetType, ctx.pool), "'");
    return nullptr;
}