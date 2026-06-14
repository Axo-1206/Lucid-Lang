/**
 * @file FromChecker.cpp
 * @brief Implementation of from block validation (implicit conversions).
 * 
 * Validates from blocks including:
 *   - Conversion signature validation
 *   - Duplicate source type detection
 *   - Inline entry body return type checking
 *   - Path entry callable validation
 */

#include "DeclChecker.hpp"
#include "semantic/checker/expr/ExprChecker.hpp"
#include "semantic/checker/stmt/StmtChecker.hpp"
#include "semantic/helpers/NameMangler.hpp"
#include "semantic/checker/TypeChecker.hpp"
#include "debug/DebugMacros.hpp"

#include <unordered_set>

void checkFromDecl(FromDeclAST* from, SemanticContext& ctx) {
    LUC_LOG_SEMANTIC_VERBOSE("checkFromDecl");
    
    // Validate attributes using the attribute registry
    decl::validateAttributes(from, ctx);
    
    // Check each entry
    std::unordered_set<std::string> sourceTypes;
    
    for (auto* entry : from->entries) {
        if (entry->kind == FromEntryKind::Inline) {
            // Inline entry – check function signature and body
            if (entry->funcType) {
                // Get source type from first parameter
                if (!entry->funcType->params.empty()) {
                    ParamAST* sourceParam = entry->funcType->params[0];
                    if (sourceParam->valueType) {
                        // Check for duplicate source type
                        std::string sourceKey = NameMangler::mangleType(sourceParam->valueType, ctx.pool);
                        if (sourceTypes.find(sourceKey) != sourceTypes.end()) {
                            ctx.error(entry->loc, DiagCode::E2001,
                                      "duplicate conversion from same source type");
                        }
                        sourceTypes.insert(sourceKey);
                    }
                }
            }
            
            // Check body
            if (entry->body) {
                // The body should return the target type
                TypeAST* expectedReturn = from->targetType;
                checkStmt(entry->body, ctx, expectedReturn);
            }
        } else if (entry->kind == FromEntryKind::Path && entry->path) {
            // Path entry – check the reference
            TypeAST* pathType = checkExpr(entry->path, ctx);
            if (pathType && !TypeChecker::isCallable(pathType, *ctx.typeResolver)) {
                ctx.error(entry->path->loc, DiagCode::E2001,
                          "path entry must reference a callable function");
            }
        }
    }
}