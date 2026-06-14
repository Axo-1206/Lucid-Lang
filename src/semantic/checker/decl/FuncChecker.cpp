/**
 * @file FuncChecker.cpp
 * @brief Implementation of function declaration validation.
 * 
 * Validates function declarations including:
 *   - Extern function restrictions (no body)
 *   - Non-extern functions must have a body
 *   - Return type checking via StmtChecker
 *   - Async qualifier validation
 */

#include "DeclChecker.hpp"
#include "semantic/checker/stmt/StmtChecker.hpp"
#include "debug/DebugMacros.hpp"

void checkFuncDecl(FuncDeclAST* func, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkFuncDecl: " << ctx.pool.lookup(func->name));
    
    // Validate attributes using the attribute registry
    decl::validateAttributes(func, ctx);
    
    // Check if this is an extern function (no body allowed)
    bool isExtern = decl::hasAttribute(func, attribute::getExternId());
    
    if (isExtern) {
        if (func->body) {
            ctx.error(func->loc, DiagCode::E2001,
                      "extern function '", ctx.pool.lookup(func->name), 
                      "' cannot have a body");
        }
        return;  // No further checks for extern functions
    }
    
    // Non-extern functions must have a body
    if (!func->body) {
        ctx.error(func->loc, DiagCode::E2001,
                  "function '", ctx.pool.lookup(func->name), "' must have a body");
        return;
    }
    
    // Push function scope for body checking
    ctx.scope.push();
    
    // Check function body with expected return type
    TypeAST* expectedReturn = func->resolvedReturnType;
    checkStmt(func->body, ctx, expectedReturn);
    
    // Pop function scope
    ctx.scope.pop();
    
    // Note: Async qualifier restrictions are checked at call sites,
    // not at declaration time. The ~async qualifier is valid on any
    // function declaration.
}