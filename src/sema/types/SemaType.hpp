/// @file SemaType.hpp
/// @brief Name lookup for the semantic analyzer.
/// 
/// All name lookup logic with proper priority and diagnostics.
/// 
/// @architectural_note Lookup Priority
///   1. Generic parameters in current scope (highest priority, shadow everything)
///   2. Value/Type declarations in local scopes (innermost to outermost)
///   3. Value/Type declarations in module scope (global)
/// 
/// @architectural_note Two namespaces
///   - VALUE NAMESPACE: variables, functions, parameters, fields, enum variants
///   - TYPE NAMESPACE: structs, enums, traits, generic params
///
/// Resolves type annotations to their semantic representations.
/// 
/// @architectural_note Types are read-only
///   The parser created all TypeAST nodes. This file resolves them by
///   looking up names in the symbol table and validating compound types.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─────────────────────────────────────────────────────────────────────────────
// Generic Parameter Lookup
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check if a name is a generic parameter in the current scope.
/// 
/// Generic parameters have the HIGHEST priority and shadow type names.
bool isGenericParam(InternedString name, SemaContext& ctx);

/// @brief Look up a generic parameter by name.
/// @return The GenericParamDeclAST if found, nullptr otherwise.
const GenericParamDeclAST* lookupGenericParam(InternedString name, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Value Lookup (variables, functions, parameters, fields, enum variants)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Look up a value declaration by name.
/// 
/// Searches: generic params → local scopes → module scope
/// Generic params are NOT values, so they don't match here.
const ValueDeclAST* lookupValue(InternedString name, SemaContext& ctx);

/// @brief Look up a value and report E2001 if not found.
const ValueDeclAST* resolveValueOrError(const IdentifierExprAST* expr, SemaContext& ctx);

/// @brief Look up a function by name.
/// 
/// Convenience wrapper that checks the resolved value is a FuncDeclAST.
const FuncDeclAST* lookupFunction(InternedString name, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Type Lookup (structs, enums, traits)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Look up a type declaration by name.
/// 
/// Searches: local scopes → module scope
/// Generic parameters are NOT type declarations (they shadow, but are separate).
const TypeDeclAST* lookupType(InternedString name, SemaContext& ctx);

/// @brief Look up a type with proper priority (generic params shadow types).
/// 
/// This is the main type resolution function. It handles:
///   1. Check if it's a generic parameter (returns nullptr, no error)
///   2. Look up as concrete type (returns TypeDeclAST*)
///   3. Not found (reports E2002, returns nullptr)
const TypeDeclAST* resolveTypeOrError(const NamedTypeAST* type, SemaContext& ctx);

/// @brief Resolve a named type reference, reporting E2002 on failure.
/// Alias for resolveTypeOrError() for consistency.
const TypeDeclAST* resolveTypeNameOrError(const NamedTypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Trait Reference Resolution
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Resolve a trait reference to its declaration.
///
/// A trait reference is a NamedTypeAST that must resolve to a TraitDeclAST.
/// This is used in:
///   - Struct declarations: `struct Entity : Vector2, Named { ... }`
///   - Generic constraints: `<T : Vector2 + Named>`
///
/// @param ref The trait reference (NamedTypeAST).
/// @param ctx The semantic context.
/// @return The resolved TraitDeclAST, or nullptr on error.
const TraitDeclAST* resolveTraitRef(const NamedTypeAST* ref, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Module Member Lookup (module:member)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Look up a member in a module's table.
/// Used for module:member access. The module must already be resolved.
const ValueDeclAST* lookupModuleMember(ModuleAST* module, InternedString memberName, SemaContext& ctx);

/// @brief Resolve a module alias and look up a member, with error reporting.
const ValueDeclAST* resolveModuleMemberOrError(ModuleAccessExprAST* access, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Callee Resolution (for function calls)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Resolve a call expression's callee to the FuncDeclAST it names.
/// 
/// Handles two callee shapes:
///   - IdentifierExprAST: Look up in value namespace
///   - ModuleAccessExprAST: Look up module alias, then member
/// 
/// Any other callee shape (curried call, function literal) returns nullptr
/// silently - the caller must check the callee's resolved type instead.
const FuncDeclAST* resolveCalleeOrError(const ExprAST* callee, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Redeclaration Checkers - Check only the current tier (not outer scopes)
// ─────────────────────────────────────────────────────────────────────────────

bool isValueRedeclared(InternedString name, SemaContext& ctx);
bool isTypeRedeclared(InternedString name, SemaContext& ctx);
bool isGenericParamRedeclared(InternedString name, SemaContext& ctx);
bool isImportAliasRedeclared(InternedString alias, SemaContext& ctx);

bool reportValueRedeclaration(const DeclAST* node, SemaContext& ctx);
bool reportTypeRedeclaration(const DeclAST* node, SemaContext& ctx);
bool reportGenericParamRedeclaration(const DeclAST* node, SemaContext& ctx);
bool reportImportAliasRedeclaration(InternedString alias, const BaseAST* node, SemaContext& ctx);




// ─────────────────────────────────────────────────────────────────────────────
// Type Resolution Entry Point
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Resolve a type annotation.
/// 
/// For NamedTypeAST: LOOKUP the name.
/// For compound types: recursively resolve inner types.
/// 
/// The parser already created all TypeAST nodes. This just validates they exist.
TypeAST* resolveType(const TypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Specific Type Resolvers
// ─────────────────────────────────────────────────────────────────────────────

/// Primitive types are always valid (built-in).
TypeAST* resolvePrimitiveType(const PrimitiveTypeAST* type, SemaContext& ctx);

/// @brief Resolve a named type.
/// 
/// LOOKUP PRIORITY (highest to lowest):
///   1. Generic parameter in current scope
///   2. Type in local scopes
///   3. Type in module scope (fallback)
/// 
/// Reports E2002 if not found in any tier.
TypeAST* resolveNamedType(const NamedTypeAST* type, SemaContext& ctx);

/// Recursively resolve array element type.
TypeAST* resolveArrayType(const ArrayTypeAST* type, SemaContext& ctx);

/// Recursively resolve inner type.
TypeAST* resolveNullableType(const NullableTypeAST* type, SemaContext& ctx);
TypeAST* resolveFallibleType(const FallibleTypeAST* type, SemaContext& ctx);
TypeAST* resolveCombinedType(const CombinedTypeAST* type, SemaContext& ctx);

/// @brief Resolve reference type.
/// 
/// Checks Downward Flow Rule:
///   - Cannot store &T in struct fields
///   - Cannot store &T in arrays
///   - Cannot return &T from functions
TypeAST* resolveRefType(const RefTypeAST* type, SemaContext& ctx);

/// Resolve pointer type - always valid (sealed conduit).
TypeAST* resolvePtrType(const PtrTypeAST* type, SemaContext& ctx);

/// Recursively resolve parameter and return types.
TypeAST* resolveFuncType(const FuncTypeAST* type, SemaContext& ctx);

// ─────────────────────────────────────────────────────────────────────────────
// Type Compatibility Helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Structural equality of two types.
bool typesEqual(const TypeAST* a, const TypeAST* b);

/// True if source value can be used where target is expected.
bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx);

bool isNullableType(const TypeAST* type);
bool isFallibleType(const TypeAST* type);

/// Strip ?/?!, return inner type.
TypeAST* unwrapNullable(TypeAST* type);
TypeAST* unwrapFallible(TypeAST* type);

bool isNumericType(const TypeAST* type);
bool isIntegerType(const TypeAST* type);
bool isFloatType(const TypeAST* type);

// ─────────────────────────────────────────────────────────────────────────────
// Type Validation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Validate that a const field's type is not nullable or fallible.
bool validateConstFieldType(const TypeAST* type, SemaContext& ctx);

/// @brief Validate that a trait field is not nullable or fallible.
bool validateTraitFieldType(const TypeAST* type, SemaContext& ctx);

/// @brief Validate reference type context (Downward Flow Rule).
bool validateRefContext(const RefTypeAST* type, SemaContext& ctx);

} // namespace sema