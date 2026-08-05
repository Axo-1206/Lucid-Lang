/// @file SemaValidate.cpp
/// @brief Implementation of semantic validation rules.

#include "SemaValidate.hpp"
#include "SemaCompare.hpp"
#include "SemaResolve.hpp"
#include "../context/SemaContext.hpp"
#include "debug/DebugUtils.hpp"
#include "core/diagnostics/Diagnostic.hpp"

#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace sema {

// ─── Internal Helper: Check if a type implements a trait ─────────────────

/// @brief Check if a type satisfies a single trait constraint.
/// 
/// This is the core trait conformance check. It verifies that the source type
/// (which must be a struct) implements the target trait by checking its traitRefs.
static bool satisfiesTraitConstraint(const TypeAST* actualType,
                                      const NamedTypeAST* requiredTrait,
                                      SemaContext& ctx) {
    if (!actualType || !requiredTrait) return false;

    // Generic parameters are placeholders - checked at instantiation time
    if (isGenericParamType(actualType, ctx)) {
        return true;
    }

    // Actual type must be a named type
    if (!actualType->isa<NamedTypeAST>()) return false;
    const NamedTypeAST* namedActual = actualType->as<NamedTypeAST>();
    const TypeDeclAST* typeDecl = ctx.lookupType(namedActual->name);
    if (!typeDecl) return false;

    // Only structs can implement traits
    if (!typeDecl->isa<StructDeclAST>()) return false;
    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // Resolve the required trait
    const TraitDeclAST* traitDecl = resolveTraitRef(requiredTrait, ctx);
    if (!traitDecl) return false;

    // Check if the struct implements the trait by looking at its traitRefs
    for (const NamedTypeAST* traitRef : structDecl->traitRefs) {
        const TraitDeclAST* resolved = resolveTraitRef(traitRef, ctx);
        if (resolved == traitDecl) {
            return true;
        }
    }

    return false;
}

/// @brief Validate a single generic parameter's constraints.
static bool validateParamConstraints(const TypeAST* actualType,
                                      const GenericParamDeclAST* param,
                                      SemaContext& ctx) {
    if (!param || !actualType) return true;
    if (param->constraints.empty()) return true;

    for (const NamedTypeAST* constraint : param->constraints) {
        if (!satisfiesTraitConstraint(actualType, constraint, ctx)) {
            ctx.diagnostics.error(DiagCode::Sem_GenericConstraint, actualType,
                                  "type does not implement trait '", 
                                  ctx.pool.lookup(constraint->name), "'");
            ctx.diagnostics.note(param, "parameter '", ctx.pool.lookup(param->name), 
                                 "' requires this trait");
            return false;
        }
    }

    return true;
}

// ─── Trait Validation ────────────────────────────────────────────────────

/// @brief Validate that a struct implements a single trait.
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
            ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, traitField,
                                  "struct '", ctx.pool.lookup(structDecl->name),
                                  "' is missing field '", ctx.pool.lookup(traitField->name),
                                  "' required by trait '", ctx.pool.lookup(traitDecl->name), "'");
            isValid = false;
            continue;
        }

        const FieldDeclAST* structField = it->second;

        // ─── 2. Check: Const-ness compatibility ────────────────────────
        if (traitField->isConst && !structField->isConst) {
            ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, structField,
                                  "trait '", ctx.pool.lookup(traitDecl->name),
                                  "' requires field '", ctx.pool.lookup(traitField->name),
                                  "' to be const, but struct declares it as mutable");
            isValid = false;
            continue;
        }

        // ─── 3. Check: Type compatibility ──────────────────────────────
        if (!structField->type || !traitField->type) {
            ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, structField,
                                  "field '", ctx.pool.lookup(traitField->name),
                                  "' has missing type information");
            isValid = false;
            continue;
        }

        if (traitField->isConst) {
            // Const fields: types must match exactly
            if (!typesEqual(structField->type, traitField->type)) {
                ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, structField,
                                      "const field '", ctx.pool.lookup(traitField->name),
                                      "' type mismatch: trait expects ",
                                      debug::typeToString(traitField->type, ctx.pool),
                                      ", struct has ",
                                      debug::typeToString(structField->type, ctx.pool));
                isValid = false;
                continue;
            }
        } else {
            // Non-const fields: allow assignable types
            if (!isAssignable(traitField->type, structField->type, ctx)) {
                ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, structField,
                                      "field '", ctx.pool.lookup(traitField->name),
                                      "' type mismatch: trait expects ",
                                      debug::typeToString(traitField->type, ctx.pool),
                                      ", struct has ",
                                      debug::typeToString(structField->type, ctx.pool));
                isValid = false;
                continue;
            }
        }

        // ─── 4. Check: Const trait field type restrictions ─────────────
        if (traitField->isConst) {
            if (isNullableType(traitField->type) || isFallibleType(traitField->type)) {
                ctx.diagnostics.error(DiagCode::Sem_TraitImplementation, traitField,
                                      "trait '", ctx.pool.lookup(traitDecl->name),
                                      "' has const field '", ctx.pool.lookup(traitField->name),
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

    bool hasConflict = false;
    for (const auto& [fieldName, reqs] : requirements) {
        if (reqs.size() <= 1) continue;

        const FieldRequirement* first = &reqs[0];
        
        for (size_t i = 1; i < reqs.size(); ++i) {
            const FieldRequirement* other = &reqs[i];

            if (first->isConst != other->isConst) {
                ctx.diagnostics.error(DiagCode::Sem_TraitConflict, structDecl,
                                      "field '", ctx.pool.lookup(fieldName),
                                      "' has conflicting const requirements: ",
                                      "trait '", ctx.pool.lookup(first->trait->name),
                                      "' requires ", first->isConst ? "const" : "mutable",
                                      ", but trait '", ctx.pool.lookup(other->trait->name),
                                      "' requires ", other->isConst ? "const" : "mutable");
                hasConflict = true;
                continue;
            }

            if (first->type && other->type) {
                if (!typesEqual(first->type, other->type)) {
                    ctx.diagnostics.error(DiagCode::Sem_TraitConflict, structDecl,
                                          "field '", ctx.pool.lookup(fieldName),
                                          "' has conflicting types: ",
                                          "trait '", ctx.pool.lookup(first->trait->name),
                                          "' expects ",
                                          debug::typeToString(first->type, ctx.pool),
                                          ", but trait '", ctx.pool.lookup(other->trait->name),
                                          "' expects ",
                                          debug::typeToString(other->type, ctx.pool));
                    hasConflict = true;
                }
            }
        }
    }

    return !hasConflict;
}

// ─── Const Validation ────────────────────────────────────────────────────

bool validateConstType(const TypeAST* type,
                        InternedString name,
                        const char* kind,
                        SemaContext& ctx) {
    if (!type) return false;

    if (isNullableType(type) || isFallibleType(type)) {
        ctx.diagnostics.error(DiagCode::Sem_ConstNullable, type,
                              "const ", kind, " '", ctx.pool.lookup(name),
                              "' must be definite (not nullable or fallible)");
        return false;
    }

    // Combined type (T?!) is also not allowed for const
    if (type->isa<CombinedTypeAST>()) {
        ctx.diagnostics.error(DiagCode::Sem_ConstNullable, type,
                              "const ", kind, " '", ctx.pool.lookup(name),
                              "' cannot be combined (T?!). Use a definite type.");
        return false;
    }

    return true;
}

bool validateConstInitializer(bool hasInit,
                               InternedString name,
                               const char* kind,
                               SemaContext& ctx) {
    if (!hasInit) {
        ctx.diagnostics.error(DiagCode::Sem_MissingInitializer, nullptr,
                              "const ", kind, " '", ctx.pool.lookup(name),
                              "' must have an initializer");
        return false;
    }
    return true;
}

// ─── Public Trait Validation ────────────────────────────────────────────

bool validateTraitImplementation(const StructDeclAST* structDecl,
                                  const TraitDeclAST* traitDecl,
                                  SemaContext& ctx) {
    return validateSingleTraitImplementationInternal(structDecl, traitDecl, ctx);
}

bool validateAllTraitImplementations(const StructDeclAST* structDecl,
                                      SemaContext& ctx) {
    if (!structDecl) return true;
    if (structDecl->traitRefs.empty()) return true;

    bool conflicts = checkTraitFieldConflictsInternal(structDecl, ctx);
    bool allValid = true;

    for (const NamedTypeAST* traitRef : structDecl->traitRefs) {
        const TraitDeclAST* trait = resolveTraitRef(traitRef, ctx);
        if (!trait) {
            allValid = false;
            continue;
        }

        if (!validateSingleTraitImplementationInternal(structDecl, trait, ctx)) {
            allValid = false;
            continue;
        }
        // No cache needed - traitRefs already stores the information
    }

    return allValid && !conflicts;
}

bool checkTraitFieldConflicts(const StructDeclAST* structDecl,
                               SemaContext& ctx) {
    return checkTraitFieldConflictsInternal(structDecl, ctx);
}

// ─── Generic Validation ──────────────────────────────────────────────────

bool validateGenericArguments(ArenaSpan<TypePtr> args,
                               ArenaSpan<GenericParamDeclPtr> params,
                               const BaseAST* useSite,
                               SemaContext& ctx) {
    if (args.size() != params.size()) {
        ctx.diagnostics.error(DiagCode::Sem_GenericArityMismatch, useSite,
                              "expected ", params.size(),
                              " generic arguments, got ", args.size());
        return false;
    }

    bool allValid = true;

    for (size_t i = 0; i < args.size(); ++i) {
        TypeAST* resolvedArg = resolveType(args[i], ctx);
        if (!resolvedArg) {
            ctx.diagnostics.error(DiagCode::Sem_InvalidGenericArg, useSite,
                                  "invalid generic argument at position ", i + 1);
            allValid = false;
            continue;
        }

        if (!validateParamConstraints(resolvedArg, params[i], ctx)) {
            allValid = false;
        }
    }

    return allValid;
}

bool validateGenericParameterUsage(ArenaSpan<GenericParamDeclPtr> params,
                                    const std::vector<const TypeAST*>& types,
                                    const BaseAST* useSite,
                                    SemaContext& ctx) {
    std::unordered_set<InternedString> usedParams;

    // Recursively find generic parameter references in a type
    std::function<void(const TypeAST*)> findParams = [&](const TypeAST* type) {
        if (!type) return;
        
        if (const NamedTypeAST* named = type->as<NamedTypeAST>()) {
            if (ctx.isGenericParam(named->name)) {
                usedParams.insert(named->name);
            }
            for (const TypePtr arg : named->genericArgs) {
                findParams(arg);
            }
            return;
        }

        if (const NullableTypeAST* nullable = type->as<NullableTypeAST>()) {
            findParams(nullable->inner); return;
        }
        if (const FallibleTypeAST* fallible = type->as<FallibleTypeAST>()) {
            findParams(fallible->inner); return;
        }
        if (const CombinedTypeAST* combined = type->as<CombinedTypeAST>()) {
            findParams(combined->inner); return;
        }
        if (const RefTypeAST* ref = type->as<RefTypeAST>()) {
            findParams(ref->inner); return;
        }
        if (const PtrTypeAST* ptr = type->as<PtrTypeAST>()) {
            findParams(ptr->inner); return;
        }
        if (const ArrayTypeAST* array = type->as<ArrayTypeAST>()) {
            findParams(array->element); return;
        }
        if (const FuncTypeAST* func = type->as<FuncTypeAST>()) {
            for (ParamAST* param : func->params) {
                findParams(param->type);
            }
            findParams(func->returnType);
            return;
        }
    };

    for (const TypeAST* type : types) {
        findParams(type);
    }

    bool allUsed = true;
    for (const GenericParamDeclAST* param : params) {
        if (usedParams.find(param->name) == usedParams.end()) {
            ctx.diagnostics.error(DiagCode::Sem_GenericParamUnused, useSite,
                                  "generic parameter '", ctx.pool.lookup(param->name),
                                  "' is not used in the declaration");
            allUsed = false;
        }
    }

    return allUsed;
}

// ─── Downward Flow Rule ──────────────────────────────────────────────────

bool validateRefContext(const RefTypeAST* type, SemaContext& ctx) {
    const TypeDeclAST* currentType = ctx.currentDefiningType();
    
    if (currentType && ctx.isDefiningType(currentType)) {
        ctx.diagnostics.error(DiagCode::Sem_RefInStruct, type,
                              "reference type (&T) cannot be stored in a struct field");
        return false;
    }

    return true;
}

} // namespace sema