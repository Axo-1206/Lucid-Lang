/**
 * @file VarChecker.cpp
 * @brief Implementation of variable declaration validation.
 * 
 * Validates let and const variable declarations including:
 *   - Const initialization requirements
 *   - Non-nullable initialization
 *   - Initializer type compatibility
 *   - Attribute validation
 */

#include "DeclChecker.hpp"
#include "semantic/checker/expr/ExprChecker.hpp"
#include "semantic/checker/TypeChecker.hpp"
#include "debug/DebugMacros.hpp"

void checkVarDecl(VarDeclAST* var, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkVarDecl: " << ctx.pool.lookup(var->name));
    
    // Validate attributes using the attribute registry
    decl::validateAttributes(var, ctx);
    
    // Check const variable initialization
    if (var->keyword == DeclKeyword::Const) {
        if (!var->init) {
            ctx.error(var->loc, DiagCode::E2001,
                      "const variable '", ctx.pool.lookup(var->name), "' must be initialized");
        } else if (!isConstExpr(var->init, ctx)) {
            ctx.error(var->init->loc, DiagCode::E2001,
                      "const variable initializer must be a constant expression");
        }
    }
    
    // Check non-nullable variable initialization
    if (var->valueType && !TypeChecker::isNullable(var->valueType, *ctx.typeResolver)) {
        if (!var->init) {
            ctx.error(var->loc, DiagCode::E2001,
                      "non-nullable variable '", ctx.pool.lookup(var->name), 
                      "' must be initialized");
        }
    }
    
    // Check initializer type compatibility
    if (var->init && var->valueType) {
        TypeAST* initType = checkExpr(var->init, ctx);
        if (initType && !TypeChecker::isAssignable(initType, var->valueType, ctx)) {
            ctx.error(var->init->loc, DiagCode::E2001,
                      "cannot initialize '", ctx.pool.lookup(var->name),
                      "' with value of incompatible type");
        }
    }
}