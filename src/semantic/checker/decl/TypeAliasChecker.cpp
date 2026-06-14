/**
 * @file TypeAliasChecker.cpp
 * @brief Implementation of type alias declaration validation.
 * 
 * Validates type aliases including:
 *   - Aliased type validity (already checked by resolver)
 *   - No cyclic aliases (handled by TypeResolver)
 *   - Attribute validation
 */

#include "DeclChecker.hpp"
#include "debug/DebugMacros.hpp"

void checkTypeAliasDecl(TypeAliasDeclAST* alias, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkTypeAliasDecl: " << ctx.pool.lookup(alias->name));
    
    // Validate attributes using the attribute registry
    decl::validateAttributes(alias, ctx);
    
    // The type resolver already resolved the alias
    // Just check that the aliased type is valid
    if (!alias->aliasedType) {
        ctx.error(alias->loc, DiagCode::E2001,
                  "type alias '", ctx.pool.lookup(alias->name), "' has no aliased type");
    }
}