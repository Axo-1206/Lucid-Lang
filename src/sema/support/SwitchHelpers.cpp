/// @file SwitchHelpers.cpp
/// @brief Implementation of switch validation helpers.

#include "SwitchHelpers.hpp"
#include "../types/SemaType.hpp"
#include "../types/SemaCompare.hpp"
#include "../context/SemaContext.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <unordered_set>

namespace sema {
namespace switch_helpers {

// ─── Exhaustiveness Checking ──────────────────────────────────────────────

bool checkExhaustiveness(SwitchStmtAST* stmt, 
                          TypeAST* subjectType, 
                          SemaContext& ctx) {
    if (!stmt || !subjectType) return false;
    
    // If there's a default clause, exhaustiveness is not required
    if (stmt->defaultBody) return true;
    
    // Only check enum types
    if (!isEnumType(subjectType, ctx)) return true;
    
    const EnumDeclAST* enumDecl = getEnumDeclFromType(subjectType, ctx);
    if (!enumDecl) return true;
    
    // Collect covered variants
    std::unordered_set<InternedString> covered = collectCoveredVariants(stmt, ctx);
    
    // Check for missing variants
    bool allCovered = true;
    for (const EnumVariantAST* variant : enumDecl->variants) {
        if (covered.find(variant->name) == covered.end()) {
            ctx.diagnostics.error(DiagCode::Sem_MissingCase, stmt,
                                  "switch on enum '", ctx.pool.lookup(enumDecl->name),
                                  "' is missing case for variant '", 
                                  ctx.pool.lookup(variant->name), "'");
            allCovered = false;
        }
    }
    
    return allCovered;
}

// ─── Helpers ──────────────────────────────────────────────────────────────

std::unordered_set<InternedString> collectCoveredVariants(
    SwitchStmtAST* stmt, 
    SemaContext& ctx) {
    
    std::unordered_set<InternedString> covered;
    if (!stmt) return covered;
    
    for (const SwitchCaseAST* caseStmt : stmt->cases) {
        for (ExprAST* value : caseStmt->values) {
            if (isEnumVariantAccess(value, ctx)) {
                InternedString variantName = getEnumVariantName(value, ctx);
                if (variantName.isValid()) {
                    covered.insert(variantName);
                }
            }
        }
    }
    
    return covered;
}

bool isEnumVariantAccess(ExprAST* value, SemaContext& ctx) {
    if (!value || !value->isa<FieldAccessExprAST>()) return false;
    
    const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
    
    if (!field->object->isa<IdentifierExprAST>()) return false;
    
    IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
    
    TypeDeclAST* decl = ctx.lookupType(id->name);
    return decl && decl->isa<EnumDeclAST>();
}

InternedString getEnumVariantName(ExprAST* value, SemaContext& ctx) {
    if (!value || !value->isa<FieldAccessExprAST>()) return InternedString();
    
    const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
    return field->fieldName;
}

} // namespace switch_helpers
} // namespace sema