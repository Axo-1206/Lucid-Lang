/// @file SemaCompare.hpp
/// @brief Type comparison and assignability checks.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─── Type Equality ───────────────────────────────────────────────────────

bool typesEqual(TypeAST* a, TypeAST* b);

// ─── Type Unwrapping ─────────────────────────────────────────────────────

TypeAST* unwrapNullable(TypeAST* type);
TypeAST* unwrapFallible(TypeAST* type);

// ─── Assignability ───────────────────────────────────────────────────────

bool isAssignable(TypeAST* target, TypeAST* source, SemaContext& ctx);

// ─── Numeric Type Helpers ───────────────────────────────────────────────

/// @brief Get the bit width of an integer type.
/// @param type The integer type.
/// @return The bit width (8, 16, 32, 64) or 0 if not an integer type.
size_t getIntegerBitWidth(TypeAST* type);

/// @brief Get the larger of two integer types (for promotion).
/// @param a First integer type.
/// @param b Second integer type.
/// @return The larger type, or nullptr if either is not an integer type.
TypeAST* getLargerIntegerType(TypeAST* a, TypeAST* b, SemaContext& ctx);

/// @brief Check if integer promotion from source to target is safe.
/// @param target The target type.
/// @param source The source type.
/// @param ctx The semantic context.
/// @return true if promotion is safe (target is larger or equal).
bool isIntegerPromotionSafe(TypeAST* target, TypeAST* source, SemaContext& ctx);

// ─── Type Predicates ─────────────────────────────────────────────────────

bool isNullableType(TypeAST* type);
bool isFallibleType(TypeAST* type);
bool isReferenceType(TypeAST* type);
bool isPointerType(TypeAST* type);
bool isPrimitiveType(TypeAST* type);
bool isBoolType(TypeAST* type);
bool isIntegerType(TypeAST* type);
bool isFloatType(TypeAST* type);
bool isNumericType(TypeAST* type);
bool isStringType(TypeAST* type);
bool isCharType(TypeAST* type);

// ─── Named Type Checks ──────────────────────────────────────────────────

bool isStructType(TypeAST* type, SemaContext& ctx);
bool isEnumType(TypeAST* type, SemaContext& ctx);
bool isTraitType(TypeAST* type, SemaContext& ctx);
bool isGenericParamType(TypeAST* type, SemaContext& ctx);

// ─── Switch Type Checks ─────────────────────────────────────────────────

bool isValidSwitchType(TypeAST* type, SemaContext& ctx);
EnumDeclAST* getEnumDeclFromType(TypeAST* type, SemaContext& ctx);
bool isSwitchCaseCompatible(ExprAST* value, 
                             TypeAST* subjectType, 
                             SemaContext& ctx);

// ─── FFI Compatibility ─────────────────────────────────────────────────

bool isValidFFIType(TypeAST* type, SemaContext& ctx);

// ─── BorrowedType ───────────────────────────────────────────────────

/// @brief Check if a type is a borrowed type (&T or [_]T).
bool isBorrowedType(TypeAST* type);

} // namespace sema