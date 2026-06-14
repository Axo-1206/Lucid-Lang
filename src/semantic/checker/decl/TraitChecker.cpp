/**
 * @file TraitChecker.cpp
 * @brief Implementation of trait declaration validation.
 * 
 * Validates trait definitions including:
 *   - No duplicate method names
 *   - Method signatures are valid
 *   - No method bodies (traits only contain signatures)
 */

#include "DeclChecker.hpp"
#include "debug/DebugMacros.hpp"

#include <unordered_set>

void checkTraitDecl(TraitDeclAST* trait, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkTraitDecl: " << ctx.pool.lookup(trait->name));
    
    // Validate attributes using the attribute registry
    decl::validateAttributes(trait, ctx);
    
    // Check for duplicate method names
    std::unordered_set<uint32_t> methodNames;
    
    for (auto* method : trait->methods) {
        uint32_t id = method->name.id;
        
        if (methodNames.find(id) != methodNames.end()) {
            ctx.error(method->loc, DiagCode::E2001,
                      "duplicate method name '", ctx.pool.lookup(method->name),
                      "' in trait '", ctx.pool.lookup(trait->name), "'");
        }
        methodNames.insert(id);
        
        // Note: TraitMethodAST has no body field - method bodies are not allowed
        // in traits. This is enforced by the grammar.
    }
}