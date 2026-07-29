/// @file GenericConstraintValidator.cpp
/// @brief Implementation of GenericConstraintValidator.

#include "GenericConstraintValidator.hpp"
#include "TraitImplementationCache.hpp"
#include "SemaContext.hpp"
#include "sema/types/SemaType.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"
#include "DebugUtils.hpp"

#include <unordered_set>

namespace sema {

// ─── Constructor ───────────────────────────────────────────────────────────

GenericConstraintValidator::GenericConstraintValidator(SemaContext& ctx)
    : m_ctx(ctx), m_cache(ctx.traitImpls) {
}

// ─── Single Constraint Validation ──────────────────────────────────────────

bool GenericConstraintValidator::satisfiesConstraint(
    const TypeAST* actualType,
    const NamedTypeAST* requiredTrait) const {
    
    if (!actualType || !requiredTrait) return false;

    // If the actual type is a generic parameter, it's a placeholder.
    // We can't check constraints on a placeholder type at compile time.
    if (isGenericParameterType(actualType)) {
        return true; // Will be checked at instantiation time
    }

    // Resolve the actual type to its declaration
    const TypeDeclAST* typeDecl = resolveTypeToDecl(actualType);
    if (!typeDecl) {
        return false;
    }

    // Only structs can implement traits
    if (!typeDecl->isa<StructDeclAST>()) {
        return false;
    }

    const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>();

    // Resolve the trait
    const TraitDeclAST* traitDecl = resolveTraitRef(requiredTrait, m_ctx);
    if (!traitDecl) {
        return false;
    }

    // Check if the struct implements the trait
    return m_cache.implements(structDecl, traitDecl);
}

ConstraintValidationResult GenericConstraintValidator::validateParamConstraints(
    const TypeAST* actualType,
    const GenericParamDeclAST* param) const {
    
    ConstraintValidationResult result;
    result.param = param;
    result.actualType = actualType;

    if (!param || !actualType) {
        result.isValid = false;
        result.message = "Invalid parameter or type";
        return result;
    }

    // If no constraints, always valid
    if (param->constraints.empty()) {
        return result;
    }

    // Check each constraint
    for (const NamedTypeAST* constraint : param->constraints) {
        if (!satisfiesConstraint(actualType, constraint)) {
            result.isValid = false;
            result.requiredTrait = constraint;
            result.message = "Type '" + typeName(actualType) + 
                            "' does not implement trait '" + 
                            typeName(constraint) + "'";
            return result;
        }
    }

    return result;
}

// ─── Multiple Constraint Validation ──────────────────────────────────────

bool GenericConstraintValidator::validateAllConstraints(
    ArenaSpan<TypePtr> args,
    ArenaSpan<GenericParamDeclPtr> params,
    const BaseAST* useSite) const {
    
    // Check arity first
    if (!arityMatches(args, params)) {
        m_ctx.error(useSite, DiagCode::E2207,
                    "expected ", std::to_string(expectedArity(params)),
                    " generic arguments, got ", std::to_string(args.size()));
        return false;
    }

    // Validate each argument
    bool allValid = true;
    for (size_t i = 0; i < args.size(); ++i) {
        const TypeAST* arg = args[i];
        const GenericParamDeclAST* param = params[i];

        // Resolve the argument type
        TypeAST* resolvedArg = resolveType(arg, m_ctx);
        if (!resolvedArg) {
            m_ctx.error(useSite, DiagCode::E2209,
                        "invalid generic argument at position ", std::to_string(i + 1));
            allValid = false;
            continue;
        }

        // Validate constraints
        ConstraintValidationResult result = validateParamConstraints(resolvedArg, param);
        if (!result.isValid) {
            reportConstraintError(result, useSite);
            allValid = false;
        }
    }

    return allValid;
}

std::vector<ConstraintValidationResult> 
GenericConstraintValidator::validateAllConstraintsDetailed(
    ArenaSpan<TypePtr> args,
    ArenaSpan<GenericParamDeclPtr> params) const {
    
    std::vector<ConstraintValidationResult> results;

    // Check arity
    if (!arityMatches(args, params)) {
        ConstraintValidationResult result;
        result.isValid = false;
        result.message = "Argument count mismatch: expected " +
                        std::to_string(expectedArity(params)) +
                        ", got " + std::to_string(args.size());
        results.push_back(result);
        return results;
    }

    // Validate each argument
    for (size_t i = 0; i < args.size(); ++i) {
        const TypeAST* arg = args[i];
        const GenericParamDeclAST* param = params[i];

        TypeAST* resolvedArg = resolveType(arg, m_ctx);
        if (!resolvedArg) {
            ConstraintValidationResult result;
            result.isValid = false;
            result.param = param;
            result.message = "Invalid generic argument at position " +
                            std::to_string(i + 1);
            results.push_back(result);
            continue;
        }

        ConstraintValidationResult result = validateParamConstraints(resolvedArg, param);
        results.push_back(result);
    }

    return results;
}

// ─── Generic Parameter Usage ─────────────────────────────────────────────

bool GenericConstraintValidator::isGenericParamUsed(
    const GenericParamDeclAST* param,
    const TypeAST* type) const {
    
    if (!param || !type) return false;

    // Recursively search the type tree for this generic parameter
    // We need to check if any NamedTypeAST references this parameter by name
    // within the current scope where the parameter is defined

    // Check if type is a NamedTypeAST that references this param
    if (const NamedTypeAST* namedType = type->as<NamedTypeAST>()) {
        // TODO: Check if namedType->name matches param->name within the scope
        // For now, we'll assume generic parameters are used if they appear in a type
        // We'll need proper scope checking here
        return true; // Placeholder
    }

    // Check nested types (array, nullable, pointer, etc.)
    if (const ArrayTypeAST* arrayType = type->as<ArrayTypeAST>()) {
        return isGenericParamUsed(param, arrayType->element);
    }
    if (const NullableTypeAST* nullableType = type->as<NullableTypeAST>()) {
        return isGenericParamUsed(param, nullableType->inner);
    }
    if (const FallibleTypeAST* fallibleType = type->as<FallibleTypeAST>()) {
        return isGenericParamUsed(param, fallibleType->inner);
    }
    if (const CombinedTypeAST* combinedType = type->as<CombinedTypeAST>()) {
        return isGenericParamUsed(param, combinedType->inner);
    }
    if (const RefTypeAST* refType = type->as<RefTypeAST>()) {
        return isGenericParamUsed(param, refType->inner);
    }
    if (const PtrTypeAST* ptrType = type->as<PtrTypeAST>()) {
        return isGenericParamUsed(param, ptrType->inner);
    }
    if (const FuncTypeAST* funcType = type->as<FuncTypeAST>()) {
        // Check params and return type
        for (ParamAST* p : funcType->params) {
            if (p->type && isGenericParamUsed(param, p->type)) {
                return true;
            }
        }
        if (funcType->returnType) {
            return isGenericParamUsed(param, funcType->returnType);
        }
    }

    return false;
}

bool GenericConstraintValidator::allGenericParamsUsed(
    ArenaSpan<GenericParamDeclPtr> params,
    const std::vector<const TypeAST*>& types,
    std::vector<const GenericParamDeclAST*>& unusedParams) const {
    
    unusedParams.clear();

    for (const GenericParamDeclAST* param : params) {
        bool used = false;
        for (const TypeAST* type : types) {
            if (isGenericParamUsed(param, type)) {
                used = true;
                break;
            }
        }
        if (!used) {
            unusedParams.push_back(param);
        }
    }

    return unusedParams.empty();
}

// ─── Generic Argument Arity ─────────────────────────────────────────────

bool GenericConstraintValidator::arityMatches(
    ArenaSpan<TypePtr> args,
    ArenaSpan<GenericParamDeclPtr> params) const {
    return args.size() == params.size();
}

size_t GenericConstraintValidator::expectedArity(
    ArenaSpan<GenericParamDeclPtr> params) const {
    return params.size();
}

// ─── Field Access on Generic Types ──────────────────────────────────────

bool GenericConstraintValidator::isFieldAccessibleOnGenericType(
    const TypeAST* genericType,
    InternedString fieldName) const {
    
    if (!genericType) return false;

    // If it's not a named type, fields aren't accessible
    if (!genericType->isa<NamedTypeAST>()) {
        return false;
    }

    const NamedTypeAST* namedType = genericType->as<NamedTypeAST>();

    // Check if this is a generic parameter
    if (isGenericParameterType(genericType)) {
        // Generic parameter must have constraints to access fields
        // We need to check if any constraint provides this field
        // TODO: Look up the generic parameter and check its constraints
        return false;
    }

    // It's a concrete type - resolve it and check fields normally
    const TypeDeclAST* typeDecl = resolveTypeToDecl(genericType);
    if (!typeDecl) return false;

    if (const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>()) {
        for (const FieldDeclAST* field : structDecl->fields) {
            if (field->name == fieldName) {
                return true;
            }
        }
    }

    return false;
}

const TypeAST* GenericConstraintValidator::getFieldTypeOnGenericType(
    const TypeAST* genericType,
    InternedString fieldName) const {
    
    if (!genericType) return nullptr;

    // Similar to isFieldAccessibleOnGenericType but returns the field type
    if (!genericType->isa<NamedTypeAST>()) {
        return nullptr;
    }

    const NamedTypeAST* namedType = genericType->as<NamedTypeAST>();

    if (isGenericParameterType(genericType)) {
        // For generic parameters, we need to check constraints
        // TODO: Look up the generic parameter and check its constraints
        return nullptr;
    }

    const TypeDeclAST* typeDecl = resolveTypeToDecl(genericType);
    if (!typeDecl) return nullptr;

    if (const StructDeclAST* structDecl = typeDecl->as<StructDeclAST>()) {
        for (const FieldDeclAST* field : structDecl->fields) {
            if (field->name == fieldName) {
                return field->type;
            }
        }
    }

    return nullptr;
}

// ─── Helper Functions ────────────────────────────────────────────────────

const TypeDeclAST* GenericConstraintValidator::resolveTypeToDecl(
    const TypeAST* type) const {
    
    if (!type) return nullptr;

    if (const NamedTypeAST* namedType = type->as<NamedTypeAST>()) {
        // Use the existing type lookup
        return lookupType(namedType->name, m_ctx);
    }

    return nullptr;
}

bool GenericConstraintValidator::isGenericParameterType(const TypeAST* type) const {
    // A type is a generic parameter if it's a NamedTypeAST that refers to a
    // generic parameter in the current scope, OR if it's a GenericParamTypeAST
    // (if we create that node type).
    
    if (!type) return false;

    // TODO: Check if the NamedTypeAST name matches a generic parameter in scope
    // For now, we'll use a simple heuristic: if the type name is a single capital letter
    // and we can't resolve it as a concrete type, it might be a generic parameter.
    // This is a placeholder - proper implementation needs scope-aware checking.
    
    if (const NamedTypeAST* namedType = type->as<NamedTypeAST>()) {
        const TypeDeclAST* resolved = lookupType(namedType->name, m_ctx);
        return resolved == nullptr;
    }

    return false;
}

std::string GenericConstraintValidator::typeName(const TypeAST* type) const {
    if (!type) return "<unknown type>";

    // Use the existing debug helper
    return debug::typeToString(type, m_ctx.pool());
}

void GenericConstraintValidator::reportConstraintError(
    const ConstraintValidationResult& result,
    const BaseAST* useSite) const {
    
    if (!result.isValid && useSite) {
        m_ctx.error(useSite, DiagCode::E2208,
                    "generic constraint violation: ", result.message);
        
        // Add a note about the parameter
        if (result.param) {
            m_ctx.note(result.param,
                       "parameter '", m_ctx.pool().lookup(result.param->name),
                       "' requires this trait");
        }
        
        // Add a note about the actual type
        if (result.actualType) {
            m_ctx.note(useSite,
                       "provided type '", typeName(result.actualType),
                       "' does not satisfy the constraint");
        }
    }
}

} // namespace sema