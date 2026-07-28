/// @file SemaType.hpp
/// @brief Type resolution for the semantic analyzer.
/// 
/// Resolves type annotations to their semantic representations.
/// 
/// @architectural_note Types are read-only
///   The parser created all TypeAST nodes. This file resolves them by
///   looking up names in the symbol table and validating compound types.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

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