/**
 * @file TypeChecker.hpp
 * @brief Type compatibility, assignment checking, and type inference.
 * 
 * ============================================================================
 * TYPE CHECKER MODULE
 * ============================================================================
 * 
 * This module provides stateless type checking utilities used throughout
 * Phase 3 (expression and statement checking). All functions are organized
 * under the TypeChecker namespace for clarity and to avoid name conflicts.
 * 
 * ─── Categories ───────────────────────────────────────────────────────────
 * 
 *   1. Equality & Assignment: isEqual, isAssignable, canPromote, canConvert
 *   2. Numeric Operations: isNumeric, isInteger, isFloat, isBoolean
 *   3. Callable Validation: isCallable, getReturnType, getParameterTypes
 *   4. Array Operations: isArray, getElementType, isSliceable
 *   5. Nullable Operations: isNullable, unwrapNullable, isNilLiteral
 *   6. Result Type Operations: isResult, getSuccessType, getErrorType
 *   7. Type Inference: unify, commonType
 *   8. Constant Evaluation: getConstantIntValue
 *   9. Type Queries: getUnderlyingType, isVoid
 * 
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * 
 *   - TypeResolver for alias unwrapping and type equality
 *   - SemanticContext for error reporting and scope access
 * 
 * ─── Usage Example ────────────────────────────────────────────────────────
 * 
 *   TypeAST* lhsType = checkExpr(lhs, ctx);
 *   TypeAST* rhsType = checkExpr(rhs, ctx);
 *   
 *   if (!TypeChecker::isAssignable(rhsType, lhsType, ctx)) {
 *       ctx.error(loc, DiagCode::E2001, "cannot assign");
 *   }
 * 
 * @see TypeResolver for type resolution
 * @see ExprChecker for expression checking
 */

#pragma once

#include "ast/TypeAST.hpp"
#include "ast/ExprAST.hpp"
#include "ast/DeclAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "semantic/resolver/TypeResolver.hpp"

#include <optional>
#include <vector>

/**
 * @brief Type checking utilities namespace.
 * 
 * All functions are stateless – they depend only on the provided
 * TypeResolver and SemanticContext parameters.
 */
namespace TypeChecker {

// ============================================================================
// Equality & Assignment
// ============================================================================

/**
 * @brief Checks if two types are exactly equal (after alias unwrapping).
 * 
 * @param a First type
 * @param b Second type
 * @param resolver Type resolver for alias unwrapping
 * @return true if types are equal
 */
bool isEqual(TypeAST* a, TypeAST* b, TypeResolver& resolver);

/**
 * @brief Checks if two types are equal for comparison (allows matching numeric types).
 * 
 * For comparison operators (==, !=, <, >, <=, >=), numeric types of different
 * signedness/width are considered comparable (e.g., int and float).
 * 
 * @param a First type
 * @param b Second type
 * @param resolver Type resolver for alias unwrapping
 * @return true if types are comparable
 */
bool typesEqualForComparison(TypeAST* a, TypeAST* b, TypeResolver& resolver);

/**
 * @brief Checks if source type can be assigned to target type.
 * 
 * Considers:
 *   - Exact equality
 *   - Numeric promotion (int → float, byte → int)
 *   - Implicit conversion (via `from` declarations)
 *   - Nil assignment to nullable types
 * 
 * @param source Source type
 * @param target Target type
 * @param ctx Semantic context (for from declarations)
 * @return true if assignment is valid
 */
bool isAssignable(TypeAST* source, TypeAST* target, SemanticContext& ctx);

/**
 * @brief Checks if source type can be promoted to target type.
 * 
 * Numeric promotions:
 *   - byte → short → int → long
 *   - int → float → double
 *   - uint → ulong
 * 
 * @param source Source type
 * @param target Target type
 * @param resolver Type resolver for type queries
 * @return true if promotion is possible
 */
bool canPromote(TypeAST* source, TypeAST* target, TypeResolver& resolver);

/**
 * @brief Checks if there's an implicit conversion via `from` declaration.
 * 
 * Looks for a from block that converts source → target.
 * 
 * @param source Source type
 * @param target Target type
 * @param ctx Semantic context (for from declarations lookup)
 * @return true if conversion exists
 */
bool canConvert(TypeAST* source, TypeAST* target, SemanticContext& ctx);

// ============================================================================
// Numeric Operations
// ============================================================================

/**
 * @brief Checks if a type is numeric (integer or float).
 * 
 * Numeric types: int types, float types, byte, short, long, etc.
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type is numeric
 */
bool isNumeric(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Checks if a type is an integer type.
 * 
 * Integer types: int, byte, short, long, and unsigned variants.
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type is integer
 */
bool isInteger(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Checks if a type is a float type.
 * 
 * Float types: float, double, decimal
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type is float
 */
bool isFloat(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Checks if a type is boolean.
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type is bool
 */
bool isBoolean(TypeAST* type, TypeResolver& resolver);

// ============================================================================
// Callable Validation
// ============================================================================

/**
 * @brief Checks if a type is callable (function or method).
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type is a function type
 */
bool isCallable(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Gets the return type of a callable type.
 * 
 * For function types, returns the first return type.
 * For non-callable types, returns nullptr.
 * 
 * @param type The callable type
 * @param resolver Type resolver for alias unwrapping
 * @return TypeAST* The return type, or nullptr
 */
TypeAST* getReturnType(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Gets the parameter types of a callable type.
 * 
 * @param type The callable type
 * @param resolver Type resolver for alias unwrapping
 * @return std::vector<TypeAST*> Parameter types (empty if not callable)
 */
std::vector<TypeAST*> getParameterTypes(TypeAST* type, TypeResolver& resolver);

// ============================================================================
// Array Operations
// ============================================================================

/**
 * @brief Checks if a type is an array type (slice, dynamic, or fixed).
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type is an array
 */
bool isArray(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Gets the element type of an array type.
 * 
 * @param type The array type
 * @param resolver Type resolver for alias unwrapping
 * @return TypeAST* The element type, or nullptr
 */
TypeAST* getElementType(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Checks if a type is sliceable (can be used in slice expression).
 * 
 * Sliceable types: slice, dynamic array, fixed array
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type can be sliced
 */
bool isSliceable(TypeAST* type, TypeResolver& resolver);

// ============================================================================
// Nullable Operations
// ============================================================================

/**
 * @brief Checks if a type is nullable (has `?` suffix).
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type is nullable
 */
bool isNullable(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Gets the inner type of a nullable type.
 * 
 * @param type The nullable type
 * @param resolver Type resolver for alias unwrapping
 * @return TypeAST* The inner type, or the original type if not nullable
 */
TypeAST* unwrapNullable(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Checks if a value is nil (null literal).
 * 
 * @param expr The expression to check
 * @return true if expression is the nil literal
 */
bool isNilLiteral(ExprAST* expr);

// ============================================================================
// Result Type Operations
// ============================================================================

/**
 * @brief Checks if a type is a result type (has `!` suffix).
 * 
 * @param type The type to check
 * @param resolver Type resolver for alias unwrapping
 * @return true if type is a result type
 */
bool isResult(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Gets the success type of a result type.
 * 
 * @param type The result type
 * @param resolver Type resolver for alias unwrapping
 * @return TypeAST* The success type, or nullptr
 */
TypeAST* getSuccessType(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Gets the error type of a result type.
 * 
 * @param type The result type
 * @param resolver Type resolver for alias unwrapping
 * @return TypeAST* The error type, or nullptr (bare `!`)
 */
TypeAST* getErrorType(TypeAST* type, TypeResolver& resolver);

// ============================================================================
// Type Inference
// ============================================================================

/**
 * @brief Unifies two types, finding the most specific common type.
 * 
 * Used for type inference in expressions like binary operations.
 * 
 * @param a First type
 * @param b Second type
 * @param ctx Semantic context
 * @return TypeAST* The unified type, or nullptr if incompatible
 */
TypeAST* unify(TypeAST* a, TypeAST* b, SemanticContext& ctx);

/**
 * @brief Finds the common type for a binary operation.
 * 
 * For numeric operations, returns the wider type.
 * For other operations, requires exact match.
 * 
 * @param a Left operand type
 * @param b Right operand type
 * @param ctx Semantic context
 * @return TypeAST* The common type, or nullptr if incompatible
 */
TypeAST* commonType(TypeAST* a, TypeAST* b, SemanticContext& ctx);

// ============================================================================
// Constant Evaluation
// ============================================================================

/**
 * @brief Extracts constant integer value from an expression (if possible).
 * 
 * @param expr The expression
 * @param ctx Semantic context
 * @return std::optional<int64_t> The integer value, or nullopt if not constant
 */
std::optional<int64_t> getConstantIntValue(ExprAST* expr, SemanticContext& ctx);

// ============================================================================
// Type Queries
// ============================================================================

/**
 * @brief Gets the underlying type after unwrapping aliases and nullable.
 * 
 * @param type The type to unwrap
 * @param resolver Type resolver for alias unwrapping
 * @return TypeAST* The unwrapped type
 */
TypeAST* getUnderlyingType(TypeAST* type, TypeResolver& resolver);

/**
 * @brief Checks if a type is void (no value).
 * 
 * @param type The type to check
 * @return true if type is void (nullptr or void type)
 */
bool isVoid(TypeAST* type);

} // namespace TypeChecker