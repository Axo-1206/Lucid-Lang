/// @file GenericValidation.hpp
/// @brief Generic argument validation helpers.
/// 
/// Provides functions for validating generic arguments against parameters
/// and checking generic parameter usage.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ArenaSpan.hpp"

#include <vector>

namespace sema {

// Forward declarations
struct SemaContext;

// ─── Generic Argument Validation ──────────────────────────────────────────

/// @brief Validate generic arguments against generic parameters.
/// 
/// Checks:
///   1. Arity matches
///   2. Each argument satisfies its parameter's constraints
/// 
/// @param args The provided generic arguments.
/// @param params The declared generic parameters.
/// @param useSite The AST node where the instantiation occurs.
/// @param ctx The semantic context.
/// @return true if all arguments are valid.
bool validateGenericArguments(ArenaSpan<TypePtr> args,
                              ArenaSpan<GenericParamDeclPtr> params,
                              const BaseAST* useSite,
                              SemaContext& ctx);

/// @brief Validate that all generic parameters are used.
/// 
/// @param params The generic parameters to check.
/// @param types The types to search in.
/// @param ctx The semantic context.
/// @param useSite The AST node for error reporting.
/// @return true if all parameters are used.
bool validateGenericParameterUsage(ArenaSpan<GenericParamDeclPtr> params,
                                    const std::vector<const TypeAST*>& types,
                                    const BaseAST* useSite,
                                    SemaContext& ctx);

/// @brief Check if a field is accessible on a generic type.
/// 
/// For generic parameters with constraints, only fields from the trait
/// constraints are accessible.
/// 
/// @param genericType The generic type (may be a named type with generic args).
/// @param fieldName The field name to check.
/// @param ctx The semantic context.
/// @return true if the field is accessible.
bool isFieldAccessibleOnGenericType(const TypeAST* genericType,
                                    InternedString fieldName,
                                    SemaContext& ctx);

/// @brief Get the type of a field on a generic type.
/// 
/// @param genericType The generic type.
/// @param fieldName The field name.
/// @param ctx The semantic context.
/// @return The field type, or nullptr if not accessible.
const TypeAST* getFieldTypeOnGenericType(const TypeAST* genericType,
                                         InternedString fieldName,
                                         SemaContext& ctx);

} // namespace sema