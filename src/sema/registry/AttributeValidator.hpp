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
/// 
/// @note Only declarations that support attributes are:
///       - FuncDeclAST
///       - StructDeclAST
///       - EnumDeclAST
///       - TraitDeclAST
///       - VarDeclAST (let/const at module level)
///       - ImportDeclAST
/// 
/// @note Parameters (ParamAST) and fields (FieldDeclAST) do NOT support
///       attributes - they have no attributes member.
bool validateAllAttributes(const DeclAST* decl, SemaContext& ctx);

/// @brief Validate a specific attribute on its owner.
/// @param attr The attribute to validate.
/// @param owner The declaration the attribute is attached to.
/// @param ctx The semantic context.
/// @return true if the attribute is valid.
/// 
/// @note This is called by validateAllAttributes for each attribute.
///       It dispatches to the appropriate validator based on attribute name.
bool validateAttribute(const AttributeAST* attr, const DeclAST* owner, SemaContext& ctx);

// ─── Individual Attribute Validators ──────────────────────────────────────

/// @brief Validate @[export] attribute.
bool validateExport(const AttributeAST* attr, const DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[foreign] attribute (only on functions).
bool validateForeign(const AttributeAST* attr, const DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[link] attribute (module-level or on functions).
bool validateLink(const AttributeAST* attr, const DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[deprecated] attribute.
bool validateDeprecated(const AttributeAST* attr, const DeclAST* owner, SemaContext& ctx);

/// @brief Validate @[inline] attribute (only on functions).
bool validateInline(const AttributeAST* attr, const DeclAST* owner, SemaContext& ctx);

// ─── Helpers ──────────────────────────────────────────────────────────────

/// @brief Validate that an attribute argument is a string literal.
bool validateStringArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx);

/// @brief Validate attribute argument count.
bool validateArgCount(const AttributeAST* attr, size_t min, size_t max, SemaContext& ctx);

/// @brief Check if a declaration supports attributes.
/// @return true if the declaration has an attributes member.
bool supportsAttributes(const DeclAST* decl);

/// @brief Check if a declaration is at module level.
bool isAtModuleLevel(const DeclAST* decl, SemaContext& ctx);

} // namespace sema