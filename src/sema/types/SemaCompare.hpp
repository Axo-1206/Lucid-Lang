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

bool typesEqual(const TypeAST* a, const TypeAST* b);

// ─── Type Unwrapping ─────────────────────────────────────────────────────

TypeAST* unwrapNullable(TypeAST* type);
TypeAST* unwrapFallible(TypeAST* type);

// ─── Assignability ───────────────────────────────────────────────────────

bool isAssignable(const TypeAST* target, const TypeAST* source, SemaContext& ctx);

// ─── Numeric Type Helpers ───────────────────────────────────────────────

/// @brief Get the bit width of an integer type.
/// @param type The integer type.
/// @return The bit width (8, 16, 32, 64) or 0 if not an integer type.
size_t getIntegerBitWidth(const TypeAST* type);

/// @brief Get the larger of two integer types (for promotion).
/// @param a First integer type.
/// @param b Second integer type.
/// @return The larger type, or nullptr if either is not an integer type.
TypeAST* getLargerIntegerType(const TypeAST* a, const TypeAST* b, SemaContext& ctx);

/// @brief Check if integer promotion from source to target is safe.
/// @param target The target type.
/// @param source The source type.
/// @param ctx The semantic context.
/// @return true if promotion is safe (target is larger or equal).
bool isIntegerPromotionSafe(const TypeAST* target, const TypeAST* source, SemaContext& ctx);

// ─── Type Predicates ─────────────────────────────────────────────────────

bool isNullableType(const TypeAST* type);
bool isFallibleType(const TypeAST* type);
bool isReferenceType(const TypeAST* type);
bool isPointerType(const TypeAST* type);
bool isPrimitiveType(const TypeAST* type);
bool isBoolType(const TypeAST* type);
bool isIntegerType(const TypeAST* type);
bool isFloatType(const TypeAST* type);
bool isNumericType(const TypeAST* type);
bool isStringType(const TypeAST* type);
bool isCharType(const TypeAST* type);

// ─── Named Type Checks ──────────────────────────────────────────────────

bool isStructType(const TypeAST* type, SemaContext& ctx);
bool isEnumType(const TypeAST* type, SemaContext& ctx);
bool isTraitType(const TypeAST* type, SemaContext& ctx);
bool isGenericParamType(const TypeAST* type, SemaContext& ctx);

// ─── Switch Type Checks ─────────────────────────────────────────────────

bool isValidSwitchType(const TypeAST* type, SemaContext& ctx);
const EnumDeclAST* getEnumDeclFromType(const TypeAST* type, SemaContext& ctx);
bool isSwitchCaseCompatible(const ExprAST* value, 
                             const TypeAST* subjectType, 
                             SemaContext& ctx);

// ─── FFI Compatibility ─────────────────────────────────────────────────

bool isValidFFIType(const TypeAST* type, SemaContext& ctx);

} // namespace sema