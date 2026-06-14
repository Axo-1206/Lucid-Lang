/**
 * @file AttributeChecker.cpp
 * @brief Implementation of attribute validation using the attribute registry.
 * 
 * This file handles all @-attribute validation including:
 *   - Looking up attributes in the registry
 *   - Validating attribute context, arguments, and custom rules
 *   - Checking mutual exclusion between conflicting attributes
 *   - Reporting unknown attributes
 */

#include "DeclChecker.hpp"

namespace decl {

void validateAttributes(DeclAST* decl, SemanticContext& ctx) {
    if (!decl || decl->attributes.empty()) {
        return;
    }
    
    // Get declaration context for attribute validation
    AttributeContext declContext = getAttributeContextForDecl(decl);
    std::string declName = getDeclName(decl, ctx.pool);
    DeclKeyword declKw = getDeclKeyword(decl);
    
    // First pass: validate each attribute individually
    for (auto* attr : decl->attributes) {
        std::string_view attrName = ctx.pool.lookup(attr->name);
        
        // Look up attribute in registry
        const AttributeEntry* entry = attribute::lookup(attrName);
        
        if (!entry) {
            // Unknown attribute - report error with list of known attributes
            ctx.error(decl->loc, DiagCode::E3001,
                      "unknown attribute '@", attrName, 
                      "'. Known attributes: ", attribute::allNames());
            continue;
        }
        
        // Validate using registry's validation function
        attribute::validateAttribute(
            *entry,
            attr->args,
            declContext,
            declName,
            declKw,
            ctx.currentFile,
            decl->loc
        );
    }
    
    // Second pass: check mutual exclusion between attribute pairs
    // O(n²) is acceptable because attribute lists are small
    for (size_t i = 0; i < decl->attributes.size(); ++i) {
        for (size_t j = i + 1; j < decl->attributes.size(); ++j) {
            InternedString id1 = decl->attributes[i]->name;
            InternedString id2 = decl->attributes[j]->name;
            
            // checkMutualExclusion reports errors directly
            attribute::checkMutualExclusion(id1, id2, decl->loc);
        }
    }
}

} // namespace decl