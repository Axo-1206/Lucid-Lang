/// @file SwitchHelpers.cpp
/// @brief Implementation of switch validation helpers.

#include "SwitchHelpers.hpp"
#include "../types/SemaType.hpp"
#include "../types/SemaCompare.hpp"
#include "../context/SemaContext.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <unordered_map>
#include <unordered_set>

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
            ctx.diagnostics.error(DiagCode::Sem_MissingCase, stmt,
                                  "switch on enum '", ctx.pool.lookup(enumDecl->name),
                                  "' is missing case for variant '", 
                                  ctx.pool.lookup(variant->name), "'");
            allCovered = false;
        }
    }
    
    return allCovered;
}

bool checkDuplicateCases(const SwitchStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;
    
    std::unordered_map<InternedString, const SwitchCaseAST*> seenValues;
    std::unordered_map<std::string, const SwitchCaseAST*> seenRanges;
    
    for (const SwitchCasePtr caseStmt : stmt->cases) {
        for (const ExprAST* value : caseStmt->values) {
            // ─── Literal values ──────────────────────────────────────────
            if (value->isa<LiteralExprAST>()) {
                const LiteralExprAST* lit = value->as<LiteralExprAST>();
                InternedString key = lit->value;  // Use the literal value as key
                
                auto it = seenValues.find(key);
                if (it != seenValues.end()) {
                    ctx.diagnostics.error(DiagCode::Sem_DuplicateCase, value,
                                          "duplicate case value '", ctx.pool.lookup(key), "'");
                    return false;
                }
                seenValues[key] = caseStmt;
                continue;
            }
            
            // ─── Enum variants ──────────────────────────────────────────
            if (isEnumVariantAccess(value, ctx)) {
                InternedString variantName = getEnumVariantName(value, ctx);
                if (variantName.isValid()) {
                    auto it = seenValues.find(variantName);
                    if (it != seenValues.end()) {
                        ctx.diagnostics.error(DiagCode::Sem_DuplicateCase, value,
                                              "duplicate case value '", ctx.pool.lookup(variantName), "'");
                        return false;
                    }
                    seenValues[variantName] = caseStmt;
                }
                continue;
            }
            
            // ─── Range values ──────────────────────────────────────────
            if (value->isa<RangeExprAST>()) {
                // TODO: Implement range duplication checking
                // This requires comparing range bounds
                continue;
            }
        }
    }
    
    return true;
}

bool checkOverlappingRanges(const SwitchStmtAST* stmt, SemaContext& ctx) {
    if (!stmt) return true;
    
    // Collect all ranges
    std::vector<std::pair<RangeExprAST*, const SwitchCaseAST*>> ranges;
    
    for (const SwitchCasePtr caseStmt : stmt->cases) {
        for (const ExprAST* value : caseStmt->values) {
            if (value->isa<RangeExprAST>()) {
                ranges.push_back({
                    const_cast<RangeExprAST*>(value->as<RangeExprAST>()),
                    caseStmt
                });
            }
        }
    }
    
    // TODO: Implement range overlap detection
    // This requires evaluating range bounds (constant folding)
    
    return true;
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
    
    if (!field->object->isa<IdentifierExprAST>()) return false;
    
    const IdentifierExprAST* id = field->object->as<IdentifierExprAST>();
    
    const TypeDeclAST* decl = ctx.lookupType(id->name);
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
    
    const TypeDeclAST* decl = ctx.lookupType(id->name);
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
        ctx.diagnostics.error(DiagCode::Sem_TypeMismatch, value,
                              "case value is not compatible with switch subject type");
        return false;
    }
    
    // For enum types, check if the variant exists
    if (isEnumType(subjectType, ctx) && isEnumVariantAccess(value, ctx)) {
        const EnumDeclAST* enumDecl = getEnumDeclFromVariantAccess(value, ctx);
        if (!enumDecl) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidSwitchType, value,
                                  "invalid enum variant in case value");
            return false;
        }
        
        InternedString variantName = getEnumVariantName(value, ctx);
        bool found = false;
        for (const EnumVariantAST* variant : enumDecl->variants) {
            if (variant->name == variantName) {
                found = true;
                break;
            }
        }
        if (!found) {
            ctx.diagnostics.error(DiagCode::Sem_UndefinedValue, value,
                                  "enum '", ctx.pool.lookup(enumDecl->name),
                                  "' has no variant '", ctx.pool.lookup(variantName), "'");
            return false;
        }
    }
    
    // For range values, validate the bounds
    if (value->isa<RangeExprAST>()) {
        const RangeExprAST* range = value->as<RangeExprAST>();
        // TODO: Validate range bounds are compatible with subject type
        return true;
    }
    
    return true;
}

} // namespace switch_helpers
} // namespace sema