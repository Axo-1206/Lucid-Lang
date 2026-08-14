/// @file registry/AttributeValidator.hpp
/// @brief Pure validation functions for attributes - no state.

#pragma once

#include "../context/SemaContext.hpp"
#include "../types/SemaCompare.hpp"
#include "../types/SemaResolve.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/registry/AttributeRegistry.hpp"
#include <string>

namespace sema {

/// @brief Validate all attributes on a declaration.
/// @param decl The declaration with attributes to validate.
/// @param ctx The semantic context.
/// @return true if all attributes are valid.
bool validateAllAttributes(DeclAST* decl, SemaContext& ctx);

/// @brief Validate a specific attribute on its owner.
/// @param attr The attribute to validate.
/// @param owner The declaration the attribute is attached to.
/// @param ctx The semantic context.
/// @return true if the attribute is valid.
bool validateAttribute(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

// ─── Individual Attribute Validators ──────────────────────────────────────

/// @brief Validate @[export] attribute.
bool validateExport(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[foreign] attribute (only on functions).
bool validateForeign(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[link] attribute (module-level or on functions).
bool validateLink(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[deprecated] attribute.
bool validateDeprecated(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[inline] and @[noinline] attributes.
bool validateInlineHint(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[specialize] attribute (only on generic functions).
bool validateSpecialize(AttributeAST* attr, DeclAST* owner, SemaContext& ctx);

// ─── Helpers ──────────────────────────────────────────────────────────────

/// @brief Validate that an attribute argument is a string literal.
bool validateStringArg(ExprAST* arg, const std::string& argName, SemaContext& ctx);

/// @brief Validate attribute argument count.
bool validateArgCount(AttributeAST* attr, size_t min, size_t max, SemaContext& ctx);

/// @brief Check if a declaration is at module level (top-level).
/// @note This is different from ctx.isAtModuleLevel() which checks the current scope.
bool isModuleLevelDeclaration(DeclAST* decl, SemaContext& ctx);

} // namespace sema