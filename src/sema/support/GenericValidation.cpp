/// @file GenericValidation.cpp
/// @brief Implementation of generic validation helpers.

#include "GenericValidation.hpp"
#include "../context/SemaContext.hpp"
#include "../context/GenericConstraintValidator.hpp"
#include "../types/SemaType.hpp"
#include "core/diagnostics/DiagnosticCodes.hpp"

namespace sema {

// ─── Public Functions ──────────────────────────────────────────────────────

bool validateGenericArguments(
    ArenaSpan<TypePtr> args,
    ArenaSpan<GenericParamDeclPtr> params,
    const BaseAST* useSite,
    SemaContext& ctx) {
    
    return ctx.constraintValidator.validateAllConstraints(args, params, useSite);
}

bool validateGenericParameterUsage(
    ArenaSpan<GenericParamDeclPtr> params,
    const std::vector<const TypeAST*>& types,
    const BaseAST* useSite,
    SemaContext& ctx) {
    
    std::vector<const GenericParamDeclAST*> unusedParams;
    bool allUsed = ctx.constraintValidator.allGenericParamsUsed(
        params, types, unusedParams);

    if (!allUsed) {
        for (const GenericParamDeclAST* param : unusedParams) {
            ctx.error(useSite, DiagCode::E2209,
                      "generic parameter '", ctx.pool().lookup(param->name),
                      "' is not used in the declaration");
        }
    }

    return allUsed;
}

bool isFieldAccessibleOnGenericType(
    const TypeAST* genericType,
    InternedString fieldName,
    SemaContext& ctx) {
    
    return ctx.constraintValidator.isFieldAccessibleOnGenericType(
        genericType, fieldName);
}

const TypeAST* getFieldTypeOnGenericType(
    const TypeAST* genericType,
    InternedString fieldName,
    SemaContext& ctx) {
    
    return ctx.constraintValidator.getFieldTypeOnGenericType(
        genericType, fieldName);
}

} // namespace sema