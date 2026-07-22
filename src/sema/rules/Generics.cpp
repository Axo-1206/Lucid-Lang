/**
 * @file Generics.cpp
 * @brief Implements Sema.hpp's "Generics & Traits" and "Self-Reference / Recursive Type Validation" sections.
 *
 * Implements:
 *   - resolveTraitRef() - Resolve a trait reference to its declaration
 *   - validateGenericParamUsage() - Verify all generic parameters are used
 *   - validateTraitImplementation() - Verify struct implements trait fields
 *   - checkGenericArgs() - Verify generic argument arity and constraints
 *   - isDirectSelfReference() - Detect infinite-size self-references
 *   - checkRecursiveFieldType() - Resolve field type and reject direct self-reference
 *
 * @architectural_note Generic parameter usage validation
 *   Unused generic parameters are a compile error. This ensures every
 *   type parameter serves a purpose in the declaration's structure.
 *
 * @architectural_note Trait implementation validation
 *   A struct implementing a trait must declare all the trait's fields
 *   with matching names, types, and const-ness. This is checked at the
 *   struct declaration site.
 *
 * @architectural_note Self-reference detection
 *   Uses ctx.definingTypes.isDefining() to check if a type is currently
 *   being defined. Direct self-references (by value) are rejected as
 *   infinite-size types. Indirect references through ptr/ref/nullable
 *   are allowed.
 */

#include "../Sema.hpp"
#include "../context/SemaContext.hpp"
#include "debug/DebugUtils.hpp"

#include <unordered_set>
#include <unordered_map>
#include <functional>

namespace sema {

// =============================================================================
// resolveTraitRef
// =============================================================================

TraitDeclAST* resolveTraitRef(TraitRefAST* ref, SemaContext& ctx) {
    if (!ref) return nullptr;

    // Look up the type by name
    TypeDeclAST* decl = ctx.symbols.lookupType(ref->name);
    if (!decl) {
        ctx.error(ref, DiagCode::E2002,
                  "undefined type '", ctx.pool().lookup(ref->name), "'");
        return nullptr;
    }

    // Verify it's a trait
    if (!decl->isa<TraitDeclAST>()) {
        ctx.error(ref, DiagCode::E2002,
                  "'", ctx.pool().lookup(ref->name), "' is not a trait");
        return nullptr;
    }

    TraitDeclAST* traitDecl = decl->as<TraitDeclAST>();

    // Check generic arguments
    if (!ref->genericArgs.empty()) {
        checkGenericArgs(ref->genericArgs, traitDecl->genericParams, ref, ctx);
    }

    return traitDecl;
}

// =============================================================================
// validateGenericParamUsage
// =============================================================================

void validateGenericParamUsage(ArenaSpan<GenericParamDeclPtr> params,
                                DeclAST* owner,
                                SemaContext& ctx) {
    if (params.empty()) return;

    // Collect all generic parameter names
    std::unordered_set<InternedString> paramNames;
    for (GenericParamDeclAST* param : params) {
        paramNames.insert(param->name);
    }

    // Track which parameters are used
    std::unordered_set<InternedString> usedParams;

    // Helper to walk a type and collect used generic parameters
    std::function<void(TypeAST*)> collectUsedParams = [&](TypeAST* type) {
        if (!type) return;

        // If it's a NamedType, check if it's a generic parameter
        if (type->isa<NamedTypeAST>()) {
            NamedTypeAST* named = type->as<NamedTypeAST>();
            if (paramNames.find(named->name) != paramNames.end()) {
                usedParams.insert(named->name);
            }
            // Recursively check generic arguments
            for (TypePtr arg : named->genericArgs) {
                collectUsedParams(arg);
            }
            return;
        }

        // Recurse into compound types
        if (type->isa<NullableTypeAST>()) {
            collectUsedParams(type->as<NullableTypeAST>()->inner);
        } else if (type->isa<FallibleTypeAST>()) {
            collectUsedParams(type->as<FallibleTypeAST>()->inner);
        } else if (type->isa<CombinedTypeAST>()) {
            collectUsedParams(type->as<CombinedTypeAST>()->inner);
        } else if (type->isa<RefTypeAST>()) {
            collectUsedParams(type->as<RefTypeAST>()->inner);
        } else if (type->isa<PtrTypeAST>()) {
            collectUsedParams(type->as<PtrTypeAST>()->inner);
        } else if (type->isa<ArrayTypeAST>()) {
            collectUsedParams(type->as<ArrayTypeAST>()->element);
        } else if (type->isa<FuncTypeAST>()) {
            FuncTypeAST* func = type->as<FuncTypeAST>();
            for (ParamAST* param : func->params) {
                collectUsedParams(param->type);
            }
            for (TypePtr ret : func->returnTypes) {
                collectUsedParams(ret);
            }
        }
    };

    // Walk the owner's structure based on its kind
    if (owner->isa<StructDeclAST>()) {
        StructDeclAST* structDecl = owner->as<StructDeclAST>();
        for (FieldDeclAST* field : structDecl->fields) {
            collectUsedParams(field->type);
        }
    } else if (owner->isa<TraitDeclAST>()) {
        TraitDeclAST* traitDecl = owner->as<TraitDeclAST>();
        for (TraitFieldDeclAST* field : traitDecl->fields) {
            collectUsedParams(field->type);
        }
    } else if (owner->isa<FuncDeclAST>()) {
        FuncDeclAST* funcDecl = owner->as<FuncDeclAST>();
        // Walk the function type
        collectUsedParams(funcDecl->funcType);
    }

    // Report unused parameters
    for (GenericParamDeclAST* param : params) {
        if (usedParams.find(param->name) == usedParams.end()) {
            ctx.error(param, DiagCode::E4001,
                      "unused generic parameter '", ctx.pool().lookup(param->name), "'");
        }
    }
}

// =============================================================================
// validateTraitImplementation
// =============================================================================

bool validateTraitImplementation(StructDeclAST* structDecl,
                                  TraitRefAST* traitRef,
                                  SemaContext& ctx) {
    if (!structDecl || !traitRef) return false;

    TraitDeclAST* trait = resolveTraitRef(traitRef, ctx);
    if (!trait) return false;

    bool allFieldsImplemented = true;

    // For each field required by the trait
    for (TraitFieldDeclAST* traitField : trait->fields) {
        bool found = false;

        // Find a matching field in the struct
        for (FieldDeclAST* structField : structDecl->fields) {
            if (structField->name != traitField->name) continue;

            found = true;

            // Verify the type matches
            if (!typesEqual(structField->type, traitField->type)) {
                // Use debug::typeToString to display the types
                std::string traitTypeStr = debug::typeToString(traitField->type, ctx.pool());
                std::string structTypeStr = debug::typeToString(structField->type, ctx.pool());

                ctx.error(structField, DiagCode::E3003,
                          "trait '", ctx.pool().lookup(trait->name),
                          "' requires field '", ctx.pool().lookup(traitField->name),
                          "' of type '", traitTypeStr,
                          "', but struct declares it as '", structTypeStr, "'");
                allFieldsImplemented = false;
                break;
            }

            // Verify const-ness matches
            if (traitField->isConst && !structField->isConst) {
                ctx.error(structField, DiagCode::E3004,
                          "trait '", ctx.pool().lookup(trait->name),
                          "' requires field '", ctx.pool().lookup(traitField->name),
                          "' to be const");
                allFieldsImplemented = false;
                break;
            }

            // Trait fields must not be nullable or fallible (already checked
            // during trait declaration analysis, but verify here too)
            if (isNullableType(structField->type) || isFallibleType(structField->type)) {
                ctx.error(structField, DiagCode::E3004,
                          "trait field '", ctx.pool().lookup(traitField->name),
                          "' must not be nullable or fallible");
                allFieldsImplemented = false;
                break;
            }

            break;
        }

        if (!found) {
            ctx.error(traitRef, DiagCode::E2001,
                      "struct '", ctx.pool().lookup(structDecl->name),
                      "' is missing required trait field '",
                      ctx.pool().lookup(traitField->name), "'");
            allFieldsImplemented = false;
        }
    }

    return allFieldsImplemented;
}

// =============================================================================
// checkGenericArgs
// =============================================================================

void checkGenericArgs(ArenaSpan<TypePtr> args,
                       ArenaSpan<GenericParamDeclPtr> params,
                       BaseAST* useSite,
                       SemaContext& ctx) {
    if (args.empty() && params.empty()) return;

    // Check arity
    if (args.size() != params.size()) {
        ctx.error(useSite, DiagCode::E3001,
                  "wrong number of generic arguments: expected ",
                  std::to_string(params.size()), ", found ",
                  std::to_string(args.size()));
        return;
    }

    // Resolve each argument and check constraints
    for (size_t i = 0; i < args.size(); ++i) {
        TypeAST* argType = resolveType(args[i], ctx);
        if (!argType) continue;

        GenericParamDeclAST* param = params[i];
        if (!param) continue;

        // Check constraints: the argument type must implement each trait
        // in the parameter's constraints.
        for (TraitRefAST* constraint : param->constraints) {
            TraitDeclAST* trait = resolveTraitRef(constraint, ctx);
            if (!trait) continue;

            // For the argument type to satisfy the constraint, it must be
            // a struct that implements the trait.
            if (!argType->isa<NamedTypeAST>()) {
                ctx.error(useSite, DiagCode::E4001,
                          "generic argument does not satisfy trait constraint '",
                          ctx.pool().lookup(trait->name), "'");
                continue;
            }

            // Look up the argument type's declaration
            NamedTypeAST* namedArg = argType->as<NamedTypeAST>();
            TypeDeclAST* argDecl = ctx.symbols.lookupType(namedArg->name);
            if (!argDecl) {
                ctx.error(useSite, DiagCode::E2002,
                          "undefined type '", ctx.pool().lookup(namedArg->name), "'");
                continue;
            }

            // Check if the argument type implements the trait
            if (!argDecl->isa<StructDeclAST>()) {
                ctx.error(useSite, DiagCode::E4001,
                          "generic argument must be a struct to satisfy trait constraint '",
                          ctx.pool().lookup(trait->name), "'");
                continue;
            }

            // Actually verify the struct implements the trait
            StructDeclAST* structDecl = argDecl->as<StructDeclAST>();
            bool implementsTrait = false;

            for (TraitRefAST* structTraitRef : structDecl->traitRefs) {
                TraitDeclAST* implementedTrait = resolveTraitRef(structTraitRef, ctx);
                if (implementedTrait && implementedTrait->name == trait->name) {
                    implementsTrait = true;
                    break;
                }
            }

            if (!implementsTrait) {
                ctx.error(useSite, DiagCode::E4001,
                          "type '", ctx.pool().lookup(namedArg->name),
                          "' does not implement trait '", ctx.pool().lookup(trait->name), "'");
            }
        }
    }
}

// =============================================================================
// isDirectSelfReference
// =============================================================================

bool isDirectSelfReference(TypeAST* fieldType, TypeDeclAST* owner, SemaContext& ctx) {
    if (!fieldType || !owner) return false;

    // Only check if the owner is currently being defined
    if (!ctx.definingTypes.isDefining(owner)) {
        return false;
    }

    // Check if the field type is a named type referencing the owner
    if (fieldType->isa<NamedTypeAST>()) {
        NamedTypeAST* named = fieldType->as<NamedTypeAST>();

        // Resolve the name to check if it's the owner
        TypeDeclAST* decl = ctx.symbols.lookupType(named->name);
        if (decl == owner) {
            // It's a direct reference to the owner (no indirection)
            return true;
        }
    }

    // For wrapper types (nullable, fallible, combined, ref, ptr, array),
    // check if the inner type is a direct self-reference.
    // Indirections like ptr<T> or T? break the infinite size cycle.
    if (fieldType->isa<NullableTypeAST>()) {
        return isDirectSelfReference(fieldType->as<NullableTypeAST>()->inner, owner, ctx);
    }
    if (fieldType->isa<FallibleTypeAST>()) {
        return isDirectSelfReference(fieldType->as<FallibleTypeAST>()->inner, owner, ctx);
    }
    if (fieldType->isa<CombinedTypeAST>()) {
        return isDirectSelfReference(fieldType->as<CombinedTypeAST>()->inner, owner, ctx);
    }
    if (fieldType->isa<ArrayTypeAST>()) {
        // Arrays are direct storage (no indirection) — they don't break the cycle
        // unless the element type is a pointer/reference.
        ArrayTypeAST* arr = fieldType->as<ArrayTypeAST>();
        return isDirectSelfReference(arr->element, owner, ctx);
    }
    if (fieldType->isa<RefTypeAST>()) {
        // References break the cycle (indirection)
        return false;
    }
    if (fieldType->isa<PtrTypeAST>()) {
        // Pointers break the cycle (indirection)
        return false;
    }

    return false;
}

// =============================================================================
// checkRecursiveFieldType
// =============================================================================

void checkRecursiveFieldType(FieldDeclAST* field, TypeDeclAST* owner, SemaContext& ctx) {
    if (!field || !owner) return;

    // Resolve the field's type
    field->type = resolveType(field->type, ctx);
    if (!field->type) return;

    // Check for direct self-reference (infinite size)
    if (isDirectSelfReference(field->type, owner, ctx)) {
        ctx.error(field, DiagCode::E3003,
                  "struct '", ctx.pool().lookup(owner->name),
                  "' contains a field of its own type directly (would be infinite size)");
    }
}

} // namespace sema