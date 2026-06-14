/**
 * @file TypeTestChecker.cpp
 * @brief Implementation of type test expression checking.
 * 
 * Handles: IsExprAST (x is Type)
 * 
 * The 'is' expression checks if a value matches a given type at runtime.
 * It always returns a boolean, and can be used for type narrowing in
 * conditional contexts (if statements, match arms, etc.).
 * 
 * ─── Semantic Rules ────────────────────────────────────────────────────────
 * 
 *   1. The left-hand side (expr) can be any expression
 *   2. The right-hand side (checkType) must be a valid type
 *   3. The expression always evaluates to bool
 *   4. In a conditional context, the type of expr is narrowed within the
 *      then-branch (e.g., if value is Circle { ... value is Circle here })
 * 
 * @example
 *   if shape is Circle {
 *       // shape is known to be Circle here
 *       let radius = shape.radius
 *   }
 */

#include "ExprChecker.hpp"
#include "debug/DebugMacros.hpp"
#include "debug/DebugUtils.hpp"

TypeAST* checkIsExpr(IsExprAST* expr, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkIsExpr");
    
    // Check the left-hand side expression
    TypeAST* exprType = checkExpr(expr->expr, ctx);
    if (!exprType) {
        ctx.error(expr->expr->loc, DiagCode::E2001,
                  "cannot determine type of expression in type test");
        return nullptr;
    }
    
    // Resolve the type we're testing against
    TypeAST* checkType = ctx.typeResolver->resolve(expr->checkType);
    if (!checkType) {
        ctx.error(expr->checkType->loc, DiagCode::E2001,
                  "invalid type in type test");
        return nullptr;
    }
    
    // Basic validation: the type being tested must be something we can
    // actually test against (struct, enum, interface/trait)
    checkType = TypeChecker::getUnderlyingType(checkType, *ctx.typeResolver);
    exprType = TypeChecker::getUnderlyingType(exprType, *ctx.typeResolver);
    
    // Check if the test is meaningful
    // For primitive types, we can only test against the exact type
    if (exprType->isa<PrimitiveTypeAST>() || checkType->isa<PrimitiveTypeAST>()) {
        // For primitives, the types must match exactly
        if (!TypeChecker::isEqual(exprType, checkType, *ctx.typeResolver)) {
            // This is a warning, not an error - it will always be false
            ctx.warning(expr->loc, DiagCode::W6002,
                        "type test will always be false: '",
                        LucDebug::formatType(exprType, ctx.pool), "' is never '",
                        LucDebug::formatType(checkType, ctx.pool), "'");
        }
    }
    
    // For user-defined types, any type test is valid (may be true or false)
    // The type checker will use this information for type narrowing
    
    // The 'is' expression itself always returns bool
    return ctx.arena.make<PrimitiveTypeAST>(PrimitiveKind::Bool);
}