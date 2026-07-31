/// @file SwitchHelpers.cpp
/// @brief Implementation of switch validation helpers.

#include "SwitchHelpers.hpp"
#include "../types/SemaType.hpp"
#include "../context/SemaContext.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

namespace sema {
namespace switch_helpers {

// ─── Exhaustiveness Checking ──────────────────────────────────────────────

bool checkExhaustiveness(const SwitchStmtAST* stmt, 
                          const TypeAST* subjectType, 
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
            ctx.error(stmt, DiagCode::E3003,
                      "switch on enum '", ctx.pool.lookup(enumDecl->name),
                      "' is missing case for variant '", 
                      ctx.pool.lookup(variant->name), "'");
            allCovered = false;
        }
    }
    
    return allCovered;
}

std::unordered_set<InternedString> collectCoveredVariants(
    const SwitchStmtAST* stmt, 
    SemaContext& ctx) {
    
    std::unordered_set<InternedString> covered;
    
    if (!stmt) return covered;
    
    for (const SwitchCasePtr caseStmt : stmt->cases) {
        for (const ExprAST* value : caseStmt->values) {
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

bool isEnumVariantAccess(const ExprAST* value, SemaContext& ctx) {
    if (!value || !value->isa<FieldAccessExprAST>()) return false;
    
    const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
    
    // Object must be an identifier
    if (!field->object->isa<IdentifierExprAST>()) return false;
    
    const IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
    
    // Object must resolve to an enum type
    const TypeDeclAST* decl = lookupType(id->name, ctx);
    return decl && decl->isa<EnumDeclAST>();
}

InternedString getEnumVariantName(const ExprAST* value, SemaContext& ctx) {
    if (!value || !value->isa<FieldAccessExprAST>()) return InternedString();
    
    const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
    return field->fieldName;
}

const EnumDeclAST* getEnumDeclFromVariantAccess(const ExprAST* value, SemaContext& ctx) {
    if (!value || !value->isa<FieldAccessExprAST>()) return nullptr;
    
    const FieldAccessExprAST* field = value->as<FieldAccessExprAST>();
    
    if (!field->object->isa<IdentifierExprAST>()) return nullptr;
    
    const IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
    
    const TypeDeclAST* decl = lookupType(id->name, ctx);
    if (!decl || !decl->isa<EnumDeclAST>()) return nullptr;
    
    return decl->as<EnumDeclAST>();
}

// ─── Case Value Validation ────────────────────────────────────────────────

bool validateCaseValue(const ExprAST* value, 
                        const TypeAST* subjectType, 
                        SemaContext& ctx) {
    if (!value || !subjectType) return false;
    
    // First, check if the value type matches the subject type
    if (!isSwitchCaseCompatible(value, subjectType, ctx)) {
        ctx.error(value, DiagCode::E3003,
                  "case value is not compatible with switch subject type");
        return false;
    }
    
    // For enum types, check if the variant exists
    if (isEnumType(subjectType, ctx) && isEnumVariantAccess(value, ctx)) {
        const EnumDeclAST* enumDecl = getEnumDeclFromVariantAccess(value, ctx);
        if (!enumDecl) {
            ctx.error(value, DiagCode::E2002,
                      "invalid enum variant in case value");
            return false;
        }
        
        // Check if variant exists (this is redundant with isSwitchCaseCompatible)
        // but provides a more specific error message
        InternedString variantName = getEnumVariantName(value, ctx);
        bool found = false;
        for (const EnumVariantAST* variant : enumDecl->variants) {
            if (variant->name == variantName) {
                found = true;
                break;
            }
        }
        if (!found) {
            ctx.error(value, DiagCode::E2001,
                      "enum '", ctx.pool.lookup(enumDecl->name),
                      "' has no variant '", ctx.pool.lookup(variantName), "'");
            return false;
        }
    }
    
    // For range values, validate the bounds
    if (value->isa<RangeExprAST>()) {
        const RangeExprAST* range = value->as<RangeExprAST>();
        // TODO: Validate range bounds are compatible with subject type
        // This requires checking lo and hi expressions
        return true;
    }
    
    return true;
}

} // namespace switch_helpers
} // namespace sema