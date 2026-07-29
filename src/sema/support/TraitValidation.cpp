/// @file TraitValidation.cpp
/// @brief Implementation of trait validation helpers.

#include "TraitValidation.hpp"
#include "../context/SemaContext.hpp"
#include "../context/TraitImplementationCache.hpp"
#include "../types/SemaType.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "debug/DebugUtils.hpp"

#include <unordered_map>
#include <unordered_set>

namespace sema {

// ─── Internal Helpers ──────────────────────────────────────────────────────

/// @brief Validate that a struct implements a single trait.
/// 
/// This is the core validation function that checks:
///   1. All trait fields exist in the struct
///   2. Field types match exactly (or are compatible)
///   3. Const-ness matches (trait const → struct const required)
static bool validateSingleTraitImplementationInternal(
    const StructDeclAST* structDecl,
    const TraitDeclAST* traitDecl,
    SemaContext& ctx) {
    
    if (!structDecl || !traitDecl) return false;

    // Build a map of struct fields for quick lookup
    std::unordered_map<InternedString, const FieldDeclAST*> structFields;
    for (const FieldDeclAST* field : structDecl->fields) {
        structFields[field->name] = field;
    }

    bool isValid = true;

    // Check each trait field
    for (const TraitFieldDeclAST* traitField : traitDecl->fields) {
        // ─── 1. Check: Field exists in struct ──────────────────────────
        auto it = structFields.find(traitField->name);
        if (it == structFields.end()) {
            ctx.error(traitField, DiagCode::E2203,
                      "struct '", ctx.pool().lookup(structDecl->name),
                      "' is missing field '", ctx.pool().lookup(traitField->name),
                      "' required by trait '", ctx.pool().lookup(traitDecl->name), "'");
            isValid = false;
            continue;
        }

        const FieldDeclAST* structField = it->second;

        // ─── 2. Check: Const-ness compatibility ────────────────────────
        // Trait const field → struct must also be const
        if (traitField->isConst && !structField->isConst) {
            ctx.error(structField, DiagCode::E2205,
                      "trait '", ctx.pool().lookup(traitDecl->name),
                      "' requires field '", ctx.pool().lookup(traitField->name),
                      "' to be const, but struct declares it as mutable");
            isValid = false;
            continue;
        }

        // Trait non-const field → struct can be const or non-const
        // (widening is allowed - const is more restrictive)

        // ─── 3. Check: Type compatibility ──────────────────────────────
        // Types must match exactly (or be assignable)
        if (!structField->type || !traitField->type) {
            ctx.error(structField, DiagCode::E2204,
                      "field '", ctx.pool().lookup(traitField->name),
                      "' has missing type information");
            isValid = false;
            continue;
        }

        // For const fields, types must match exactly (no widening)
        if (traitField->isConst) {
            if (!typesEqual(structField->type, traitField->type)) {
                ctx.error(structField, DiagCode::E2204,
                          "const field '", ctx.pool().lookup(traitField->name),
                          "' type mismatch: trait expects ",
                          debug::typeToString(traitField->type, ctx.pool()),
                          ", struct has ",
                          debug::typeToString(structField->type, ctx.pool()));
                isValid = false;
                continue;
            }
        } else {
            // For non-const fields, allow assignable types
            if (!isAssignable(traitField->type, structField->type, ctx)) {
                ctx.error(structField, DiagCode::E2204,
                          "field '", ctx.pool().lookup(traitField->name),
                          "' type mismatch: trait expects ",
                          debug::typeToString(traitField->type, ctx.pool()),
                          ", struct has ",
                          debug::typeToString(structField->type, ctx.pool()));
                isValid = false;
                continue;
            }
        }

        // ─── 4. Check: Trait field type restrictions ────────────────────
        // Const trait fields must be definite (not nullable/fallible)
        // This is enforced at the trait declaration level, but check again here
        if (traitField->isConst) {
            if (isNullableType(traitField->type) || isFallibleType(traitField->type)) {
                // This should have been caught in analyzeTraitDecl
                ctx.error(traitField, DiagCode::E3004,
                          "trait '", ctx.pool().lookup(traitDecl->name),
                          "' has const field '", ctx.pool().lookup(traitField->name),
                          "' that is nullable or fallible (must be definite)");
                isValid = false;
                continue;
            }
        }
    }

    return isValid;
}

/// @brief Check for conflicting field names across multiple traits.
static bool checkTraitFieldConflictsInternal(
    const StructDeclAST* structDecl,
    SemaContext& ctx) {
    
    if (!structDecl) return true;

    // Collect all field requirements by name
    // Map: field name → list of (trait, const-ness, type)
    struct FieldRequirement {
        const TraitDeclAST* trait;
        bool isConst;
        const TypeAST* type;
    };
    
    std::unordered_map<InternedString, std::vector<FieldRequirement>> requirements;

    for (const NamedTypeAST* traitRef : structDecl->traitRefs) {
        const TraitDeclAST* trait = resolveTraitRef(traitRef, ctx);
        if (!trait) continue;

        for (const TraitFieldDeclAST* field : trait->fields) {
            requirements[field->name].push_back({
                trait,
                field->isConst,
                field->type
            });
        }
    }

    // Check for conflicts
    bool hasConflict = false;
    for (const auto& [fieldName, reqs] : requirements) {
        if (reqs.size() <= 1) continue;

        // Check if all requirements are compatible
        const FieldRequirement* first = &reqs[0];
        
        for (size_t i = 1; i < reqs.size(); ++i) {
            const FieldRequirement* other = &reqs[i];

            // Check const-ness compatibility
            if (first->isConst != other->isConst) {
                ctx.error(structDecl, DiagCode::E2206,
                          "field '", ctx.pool().lookup(fieldName),
                          "' has conflicting const requirements: ",
                          "trait '", ctx.pool().lookup(first->trait->name),
                          "' requires ", first->isConst ? "const" : "mutable",
                          ", but trait '", ctx.pool().lookup(other->trait->name),
                          "' requires ", other->isConst ? "const" : "mutable");
                hasConflict = true;
                continue;
            }

            // Check type compatibility
            if (first->type && other->type) {
                if (!typesEqual(first->type, other->type)) {
                    ctx.error(structDecl, DiagCode::E2206,
                              "field '", ctx.pool().lookup(fieldName),
                              "' has conflicting types: ",
                              "trait '", ctx.pool().lookup(first->trait->name),
                              "' expects ",
                              debug::typeToString(first->type, ctx.pool()),
                              ", but trait '", ctx.pool().lookup(other->trait->name),
                              "' expects ",
                              debug::typeToString(other->type, ctx.pool()));
                    hasConflict = true;
                }
            }
        }
    }

    return !hasConflict;
}

// ─── Public Functions ──────────────────────────────────────────────────────

bool validateSingleTraitImplementation(
    const StructDeclAST* structDecl,
    const TraitDeclAST* traitDecl,
    SemaContext& ctx) {
    
    return validateSingleTraitImplementationInternal(structDecl, traitDecl, ctx);
}

bool validateAllTraitImplementations(
    const StructDeclAST* structDecl,
    SemaContext& ctx) {
    
    if (!structDecl) return true;
    if (structDecl->traitRefs.empty()) return true;

    // First, check for field conflicts across traits
    bool conflicts = checkTraitFieldConflictsInternal(structDecl, ctx);

    bool allValid = true;

    // Validate each trait implementation
    for (const NamedTypeAST* traitRef : structDecl->traitRefs) {
        const TraitDeclAST* trait = resolveTraitRef(traitRef, ctx);
        if (!trait) {
            allValid = false;
            continue;
        }

        // Validate the implementation
        if (!validateSingleTraitImplementationInternal(structDecl, trait, ctx)) {
            allValid = false;
            continue;
        }

        // If validation passed, register the implementation in the cache
        ctx.traitImpls.addImplementation(structDecl, trait);
    }

    return allValid && !conflicts;
}

bool checkTraitFieldConflicts(
    const StructDeclAST* structDecl,
    SemaContext& ctx) {
    return checkTraitFieldConflictsInternal(structDecl, ctx);
}

} // namespace sema