/// @file SemaCompare.hpp
/// @brief Type comparison and assignability checks.
/// 
/// These functions compare types structurally and check if one type
/// can be used where another is expected.
/// 
/// @comparison_rules
///   1. Identical types → equal
///   2. Different shapes → not equal
///   3. Wrapper types (T?, T!, T?!) → compare inner types
///   4. Named types → compare name and generic arguments
/// 
/// @assignability_rules
///   1. Identical types → assignable
///   2. T → T? / T! / T?! (widening)
///   3. T? / T! → T?! (combining sentinels)
///   4. Struct → Trait (trait conformance)
/// 
/// @example
///   // Check if two types are equal
///   if (typesEqual(typeA, typeB)) { ... }
///   
///   // Check if source can be assigned to target
///   if (isAssignable(target, source, ctx)) { ... }
///   
///   // Check type predicates
///   if (isNullableType(type)) { ... }

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─── Type Equality ───────────────────────────────────────────────────────

/// @brief Structural equality of two TypeAST nodes.
/// 
/// Returns true if types have the same shape and content.
/// This is a deep equality check that recurses into nested types.
/// 
/// @param a First type to compare.
/// @param b Second type to compare.
/// @return true if the types are structurally equal.
bool typesEqual(const TypeAST* a, const TypeAST* b);

// ─── Type Unwrapping ─────────────────────────────────────────────────────

/// @brief Strip one layer of ? or ?!, return inner type.
TypeAST* unwrapNullable(TypeAST* type);

/// @brief Strip one layer of ! or ?!, return inner type.
TypeAST* unwrapFallible(TypeAST* type);

// ─── Assignability ───────────────────────────────────────────────────────

/// @brief Check if a value of type `source` can be used where `target` is expected.
/// 
/// Assignability rules:
///   1. Identical types → true
///   2. T → T? / T! / T?! (widening)
///   3. T? / T! → T?! (combining sentinels)
///   4. Struct → Trait (trait conformance)
///   5. Everything else → false
bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx);

// ─── Type Predicates ─────────────────────────────────────────────────────

/// @name Sentinel Checks
/// @{
bool isNullableType(const TypeAST* type);
bool isFallibleType(const TypeAST* type);
/// @}

/// @name Category Checks
/// @{
bool isReferenceType(const TypeAST* type);
bool isPointerType(const TypeAST* type);
bool isPrimitiveType(const TypeAST* type);
bool isBoolType(const TypeAST* type);
bool isIntegerType(const TypeAST* type);
bool isFloatType(const TypeAST* type);
bool isNumericType(const TypeAST* type);
bool isStringType(const TypeAST* type);
bool isCharType(const TypeAST* type);
/// @}

/// @name Named Type Checks
/// 
/// These functions check if a NamedTypeAST resolves to a specific kind of
/// declaration (struct, enum, trait, or generic parameter).
/// @{
bool isStructType(const TypeAST* type, SemaContext& ctx);
bool isEnumType(const TypeAST* type, SemaContext& ctx);
bool isTraitType(const TypeAST* type, SemaContext& ctx);
bool isGenericParamType(const TypeAST* type, SemaContext& ctx);
/// @}

// ─── Switch Type Checks ─────────────────────────────────────────────────

/// Check if a type is valid for switch statements.
/// Valid switch types: integers, bool, char, string, enums.
bool isValidSwitchType(const TypeAST* type, SemaContext& ctx);

/// Get the enum declaration from an enum type.
const EnumDeclAST* getEnumDeclFromType(const TypeAST* type, SemaContext& ctx);

/// Check if a case value is compatible with a switch subject type.
bool isSwitchCaseCompatible(const ExprAST* value, 
                             const TypeAST* subjectType, 
                             SemaContext& ctx);

// ─── FFI Compatibility ───────────────────────────────────────────────────

/// Check if a type is legal at an FFI boundary.
bool isValidFFIType(const TypeAST* type, SemaContext& ctx);

} // namespace sema