/**
 * @file StructChecker.cpp
 * @brief Implementation of struct declaration validation.
 * 
 * Validates struct definitions including:
 *   - No duplicate field names
 *   - Field default values match field types
 *   - Warning for fields with defaults appearing before non-default fields
 */

#include "DeclChecker.hpp"
#include "semantic/checker/expr/ExprChecker.hpp"
#include "semantic/checker/TypeChecker.hpp"
#include "debug/DebugMacros.hpp"

#include <unordered_set>

void checkStructDecl(StructDeclAST* structDecl, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_EXTREME("checkStructDecl: " << ctx.pool.lookup(structDecl->name));
    
    // Validate attributes using the attribute registry
    decl::validateAttributes(structDecl, ctx);
    
    // Check for duplicate field names
    std::unordered_set<uint32_t> fieldNames;
    
    for (auto* field : structDecl->fields) {
        uint32_t id = field->name.id;
        
        if (fieldNames.find(id) != fieldNames.end()) {
            ctx.error(field->loc, DiagCode::E2001,
                      "duplicate field name '", ctx.pool.lookup(field->name),
                      "' in struct '", ctx.pool.lookup(structDecl->name), "'");
        }
        fieldNames.insert(id);
        
        // Check default value type if present
        if (field->defaultVal && field->valueType) {
            TypeAST* defaultType = checkExpr(field->defaultVal, ctx);
            if (defaultType && !TypeChecker::isAssignable(defaultType, field->valueType, ctx)) {
                ctx.error(field->defaultVal->loc, DiagCode::E2001,
                          "default value type does not match field type");
            }
        }
    }
    
    // Style warning: fields with defaults should come after fields without defaults
    // This improves readability and consistency
    bool seenDefault = false;
    for (auto* field : structDecl->fields) {
        if (field->defaultVal) {
            seenDefault = true;
        } else if (seenDefault) {
            ctx.warning(field->loc, DiagCode::W6001,
                        "fields without default values should come before fields with defaults");
        }
    }
}