/// @file GenericConstraintValidator.hpp
/// @brief Validates that types satisfy generic constraints.
/// 
/// During generic instantiation (function calls, struct literals), we need
/// to verify that the provided type arguments satisfy all constraints
/// declared on the generic parameters.
/// 
/// @architectural_note This validator works closely with TraitImplementationCache
///   to check trait satisfaction for struct types.

#pragma once

#include "TraitImplementationCache.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <vector>
#include <string>

namespace sema {

// Forward declarations
struct SemaContext;

/// @brief Result of a constraint validation attempt.
struct ConstraintValidationResult {
    bool isValid = true;                    ///< True if all constraints are satisfied.
    std::string message;                    ///< Error message if invalid.
    const GenericParamDeclAST* param = nullptr;  ///< The parameter that failed.
    const NamedTypeAST* requiredTrait = nullptr; ///< The trait that wasn't satisfied.
    const TypeAST* actualType = nullptr;    ///< The type that was provided.
};

/// @brief Validates generic constraints.
/// 
/// Provides functions to check if a type satisfies the constraints of a
/// generic parameter, and to validate all parameters at once.
class GenericConstraintValidator {
public:
    /// @brief Constructor with cache reference.
    /// 
    /// @param ctx The semantic context (for error reporting).
    explicit GenericConstraintValidator(SemaContext& ctx);

    // ─── Single Constraint Validation ──────────────────────────────────────

    /// @brief Check if a type satisfies a single constraint.
    /// 
    /// @param actualType The type being checked.
    /// @param requiredTrait The trait that must be implemented.
    /// @return true if the type implements the trait.
    bool satisfiesConstraint(const TypeAST* actualType,
                              const NamedTypeAST* requiredTrait) const;

    /// @brief Check if a type satisfies all constraints of a generic parameter.
    /// 
    /// @param actualType The type being checked.
    /// @param param The generic parameter with constraints.
    /// @return Validation result with detailed error information.
    ConstraintValidationResult validateParamConstraints(
        const TypeAST* actualType,
        const GenericParamDeclAST* param) const;

    // ─── Multiple Constraint Validation ────────────────────────────────────

    /// @brief Validate that all generic arguments satisfy their parameters.
    /// 
    /// @param args The types being provided.
    /// @param params The generic parameters with constraints.
    /// @param useSite The AST node where the instantiation occurs (for errors).
    /// @return true if all constraints are satisfied.
    bool validateAllConstraints(
        ArenaSpan<TypePtr> args,
        ArenaSpan<GenericParamDeclPtr> params,
        const BaseAST* useSite) const;

    /// @brief Validate that all generic arguments satisfy their parameters,
    ///        with detailed error messages.
    /// 
    /// @param args The types being provided.
    /// @param params The generic parameters with constraints.
    /// @return Vector of validation results for each parameter.
    std::vector<ConstraintValidationResult> validateAllConstraintsDetailed(
        ArenaSpan<TypePtr> args,
        ArenaSpan<GenericParamDeclPtr> params) const;

    // ─── Generic Parameter Usage ───────────────────────────────────────────

    /// @brief Check if a generic parameter is used in a type.
    /// 
    /// Used to detect unused generic parameters.
    /// 
    /// @param param The generic parameter to check.
    /// @param type The type to search in.
    /// @return true if the parameter is used in the type.
    bool isGenericParamUsed(const GenericParamDeclAST* param,
                             const TypeAST* type) const;

    /// @brief Check if all generic parameters are used in a declaration.
    /// 
    /// @param params The generic parameters to check.
    /// @param types The types to search in (fields, return types, etc.).
    /// @param unusedParams Output vector of unused parameters.
    /// @return true if all parameters are used.
    bool allGenericParamsUsed(
        ArenaSpan<GenericParamDeclPtr> params,
        const std::vector<const TypeAST*>& types,
        std::vector<const GenericParamDeclAST*>& unusedParams) const;

    // ─── Generic Argument Arity ────────────────────────────────────────────

    /// @brief Check if the number of arguments matches the number of parameters.
    /// 
    /// @param args The provided arguments.
    /// @param params The declared parameters.
    /// @return true if counts match.
    bool arityMatches(ArenaSpan<TypePtr> args,
                      ArenaSpan<GenericParamDeclPtr> params) const;

    /// @brief Get the expected number of arguments for a set of parameters.
    size_t expectedArity(ArenaSpan<GenericParamDeclPtr> params) const;

    // ─── Field Access on Generic Types ─────────────────────────────────────

    /// @brief Check if a field is accessible on a generic type.
    /// 
    /// For unconstrained generic types, no fields are accessible.
    /// For constrained generic types, only fields declared in the trait(s)
    /// are accessible.
    /// 
    /// @param genericType The generic type (may be NamedTypeAST with generic args).
    /// @param fieldName The field name to check.
    /// @return true if the field is accessible.
    bool isFieldAccessibleOnGenericType(const TypeAST* genericType,
                                        InternedString fieldName) const;

    /// @brief Get the type of a field on a generic type.
    /// 
    /// @param genericType The generic type.
    /// @param fieldName The field name.
    /// @return The field type, or nullptr if not accessible.
    const TypeAST* getFieldTypeOnGenericType(const TypeAST* genericType,
                                             InternedString fieldName) const;

private:
    // ─── Members ──────────────────────────────────────────────────────────

    SemaContext& m_ctx;
    TraitImplementationCache& m_cache;

    // ─── Helper Functions ──────────────────────────────────────────────────

    /// @brief Resolve a type to its declaration.
    /// 
    /// @param type The type to resolve.
    /// @return The TypeDeclAST, or nullptr if not a named type.
    const TypeDeclAST* resolveTypeToDecl(const TypeAST* type) const;

    /// @brief Check if a type is a generic parameter (placeholder).
    bool isGenericParameterType(const TypeAST* type) const;

    /// @brief Get the name of a type for error messages.
    std::string typeName(const TypeAST* type) const;

    /// @brief Report a constraint violation error.
    void reportConstraintError(const ConstraintValidationResult& result,
                               const BaseAST* useSite) const;
};

} // namespace sema