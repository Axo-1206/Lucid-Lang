/// @file SemaLookup.hpp
/// @brief Pure name lookup functions - find declarations by name.
/// 
/// These functions ONLY look up names in the symbol table.
/// They do NOT resolve types or validate semantics.
/// 
/// @lookup_priority
///   1. Generic parameters in current scope (highest priority)
///   2. Value/Type declarations in local scopes (innermost to outermost)
///   3. Value/Type declarations in module scope (global)

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─── Generic Parameter Lookup ───────────────────────────────────────────

/// @brief Check if a name is a generic parameter in the current scope.
/// 
/// Generic parameters have the HIGHEST priority and shadow type names.
/// 
/// @param name The name to check.
/// @param ctx The semantic context.
/// @return true if the name is a generic parameter in scope.
inline bool isGenericParam(InternedString name, SemaContext& ctx) {
    return ctx.isGenericParam(name);
}

/// @brief Look up a generic parameter by name.
inline const GenericParamDeclAST* lookupGenericParam(InternedString name, SemaContext& ctx) {
    return ctx.lookupGenericParam(name);
}

// ─── Value Lookup ─────────────────────────────────────────────────────────

/// @brief Look up a value declaration by name.
/// 
/// Searches: local scopes (innermost to outermost) → module scope
/// 
/// Value declarations include:
///   - Variables (VarDeclAST)
///   - Functions (FuncDeclAST)
///   - Parameters (ParamAST)
///   - Fields (FieldDeclAST)
///   - Enum variants (EnumVariantAST)
/// 
/// @param name The name to look up.
/// @param ctx The semantic context.
/// @return The ValueDeclAST if found, nullptr otherwise.
inline const ValueDeclAST* lookupValue(InternedString name, SemaContext& ctx) {
    return ctx.lookupValue(name);
}

/// @brief Look up a function by name.
/// 
/// Convenience wrapper that checks the resolved value is a FuncDeclAST.
inline const FuncDeclAST* lookupFunction(InternedString name, SemaContext& ctx) {
    return ctx.lookupFunction(name);
}

// ─── Type Lookup ─────────────────────────────────────────────────────────

/// @brief Look up a type declaration by name.
/// 
/// Searches: local scopes (innermost to outermost) → module scope
/// 
/// Type declarations include:
///   - Structs (StructDeclAST)
///   - Enums (EnumDeclAST)
///   - Traits (TraitDeclAST)
/// 
/// @note Generic parameters are NOT type declarations. They shadow type
///       names in scopes. Use isGenericParam() to check for them.
inline const TypeDeclAST* lookupType(InternedString name, SemaContext& ctx) {
    return ctx.lookupType(name);
}

// ─── Module Lookup ───────────────────────────────────────────────────────

/// @brief Look up a module by its import alias.
inline ModuleAST* lookupImportAlias(InternedString alias, SemaContext& ctx) {
    return ctx.lookupImport(alias);
}

/// @brief Look up a member in a module's table.
const ValueDeclAST* lookupModuleMember(ModuleAST* module, 
                                        InternedString memberName, 
                                        SemaContext& ctx);

// ─── Redeclaration Checks ────────────────────────────────────────────────

/// @name Redeclaration Checks
/// 
/// These functions check if a name is already declared in the CURRENT tier:
/// - If at module level: checks the module table
/// - If in a scope: checks only the innermost scope (not outer scopes)
/// @{

/// Check if a value name is already declared in the current tier.
bool isValueRedeclared(InternedString name, SemaContext& ctx);

/// Check if a type name is already declared in the current tier.
bool isTypeRedeclared(InternedString name, SemaContext& ctx);

/// Check if a generic parameter name is already declared in the current tier.
bool isGenericParamRedeclared(InternedString name, SemaContext& ctx);

/// Check if an import alias is already declared in the current module.
bool isImportAliasRedeclared(InternedString alias, SemaContext& ctx);

/// @}

// ─── Redeclaration Reporting ─────────────────────────────────────────────

/// @name Redeclaration Reporting
/// 
/// These functions check for redeclaration and report an error if found.
/// Returns true if a redeclaration was found (error reported).
/// @{

/// Check and report value redeclaration.
bool reportValueRedeclaration(const DeclAST* node, SemaContext& ctx);

/// Check and report type redeclaration.
bool reportTypeRedeclaration(const DeclAST* node, SemaContext& ctx);

/// Check and report generic parameter redeclaration.
bool reportGenericParamRedeclaration(const DeclAST* node, SemaContext& ctx);

/// Check and report import alias redeclaration.
bool reportImportAliasRedeclaration(InternedString alias, 
                                     const BaseAST* node, 
                                     SemaContext& ctx);

/// @}

} // namespace sema