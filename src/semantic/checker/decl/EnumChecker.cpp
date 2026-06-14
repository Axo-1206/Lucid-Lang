/**
 * @file EnumChecker.cpp
 * @brief Implementation of enum declaration validation.
 * 
 * Validates enum definitions including:
 *   - No duplicate variant names
 *   - No duplicate explicit values
 *   - Auto-assignment of values to variants without explicit values
 */

#include "DeclChecker.hpp"
#include "debug/DebugMacros.hpp"

#include <unordered_set>

void checkEnumDecl(EnumDeclAST* enumDecl, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_VERBOSE("checkEnumDecl: " << ctx.pool.lookup(enumDecl->name));
    
    // Validate attributes using the attribute registry
    decl::validateAttributes(enumDecl, ctx);
    
    // Track variant names and explicit values
    std::unordered_set<uint32_t> variantNames;
    std::unordered_set<int64_t> explicitValues;
    int64_t nextAutoValue = 0;
    
    for (auto* variant : enumDecl->variants) {
        // Check for duplicate variant name
        uint32_t id = variant->name.id;
        if (variantNames.find(id) != variantNames.end()) {
            ctx.error(variant->loc, DiagCode::E2001,
                      "duplicate variant name '", ctx.pool.lookup(variant->name),
                      "' in enum '", ctx.pool.lookup(enumDecl->name), "'");
        }
        variantNames.insert(id);
        
        // Handle explicit value
        if (variant->explicitValue.has_value()) {
            int64_t val = variant->explicitValue.value();
            
            // Check for duplicate explicit value
            if (explicitValues.find(val) != explicitValues.end()) {
                ctx.error(variant->loc, DiagCode::E2001,
                          "duplicate explicit value ", val,
                          " in enum '", ctx.pool.lookup(enumDecl->name), "'");
            }
            explicitValues.insert(val);
            nextAutoValue = val + 1;
        } else {
            // Auto-assign value, skipping any values already taken
            while (explicitValues.find(nextAutoValue) != explicitValues.end()) {
                nextAutoValue++;
            }
            variant->explicitValue = nextAutoValue;
            nextAutoValue++;
        }
    }
}