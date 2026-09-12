/// @file sema/types/GenericHelpers.hpp
/// @brief Helpers for checking generic type usage and validation.
///
/// This file provides utilities for determining if a type is a generic parameter
/// or contains generic parameters.
///
/// ─── Key Functions ──────────────────────────────────────────────────────────
/// - isGenericParameterType()   - Check if a type is T, U, etc.
/// - containsGenericParameter() - Check if a type contains any generic params
/// - isConcreteType()           - Check if a type is fully concrete

#pragma once

#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "sema/context/SemaContext.hpp"

namespace sema {

// ─── Generic Parameter Detection ──────────────────────────────────────────

/// @brief Check if a type is a generic parameter (T, U, etc.).
///
/// A type is a generic parameter if:
/// - It's a NamedTypeAST whose resolvedDecl is a GenericParamDeclAST
/// - OR it's a NamedTypeAST whose name matches a generic param in the current scope
///
/// @param type The type to check.
/// @param ctx The semantic context.
/// @return true if the type is a generic parameter.
bool isGenericParameterType(TypeAST* type, SemaContext& ctx);

/// @brief Check if a type contains any generic parameters at any depth.
///
/// This recursively walks through the type structure looking for any
/// generic parameters.
///
/// @param type The type to check.
/// @param ctx The semantic context.
/// @return true if the type contains any generic parameters.
bool containsGenericParameter(TypeAST* type, SemaContext& ctx);

/// @brief Check whether a type is fully concrete.
bool isConcreteType(TypeAST* type, SemaContext& ctx);

} // namespace sema