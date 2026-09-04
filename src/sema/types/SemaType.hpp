/// @file SemaType.hpp
/// @brief Type resolution, predicates, equality, assignability, and validation.
/// 
/// This file consolidates all type-related functionality into one header.
/// 
/// # Quick Reference
/// 
/// ## Type Resolution
///   - resolveType()              - Main entry point
///   - resolveNamedType()         - Resolve named types (including built-ins)
///   - resolveFuncType()          - Resolve function types
///   - resolveTraitRef()          - Resolve trait references
/// 
/// ## Type Predicates
///   - isIntegerType(), isFloatType(), isNumericType()
///   - isNullableType(), isFallibleType()
///   - isReferenceType(), isPointerType(), isBorrowedType()
///   - isStructType(), isEnumType(), isTraitType()
///   - isArenaType(), isArenaDescriptorType()
/// 
/// ## Type Equality & Assignability
///   - typesEqual()               - Structural equality
///   - isAssignable()             - Type compatibility for assignment
/// 
/// ## Type Unwrapping
///   - unwrapNullable(), unwrapFallible()
/// 
/// ## Self-Reference Detection
///   - checkLetSelfReference()
///   - isValidStructSelfReference()
/// 
/// ## Validation
///   - validateConstType()
///   - validateGenericArguments()
///   - validateBorrowedContext()
///   - validateForeignFunction()
///   - validateArenaInitializer()

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/memory/ArenaSpan.hpp"
#include "../context/SemaContext.hpp"

#include <vector>
#include <unordered_set>
#include <functional>

namespace sema {

// =============================================================================
// TYPE RESOLUTION
// =============================================================================

/// @brief Main entry point for type resolution.
TypeAST* resolveType(TypeAST* type, SemaContext& ctx);

/// @brief Resolve a primitive type (always succeeds).
TypeAST* resolvePrimitiveType(PrimitiveTypeAST* type, SemaContext& ctx);

/// @brief Resolve a named type (including built-ins like Arena/ArenaDescriptor).
TypeAST* resolveNamedType(NamedTypeAST* type, SemaContext& ctx);

/// @brief Resolve built-in types (Arena, ArenaDescriptor, Simd).
TypeAST* resolveBuiltinType(TypeAST* type, SemaContext& ctx);

/// @brief Resolve a qualified type access: module:Type.
TypeAST* resolveModuleTypeAccess(ModuleTypeAccessAST* type, SemaContext& ctx);

/// @brief Resolve an array type.
TypeAST* resolveArrayType(ArrayTypeAST* type, SemaContext& ctx);

/// @brief Resolve a nullable type (T?).
TypeAST* resolveNullableType(NullableTypeAST* type, SemaContext& ctx);

/// @brief Resolve a fallible type (T!).
TypeAST* resolveFallibleType(FallibleTypeAST* type, SemaContext& ctx);

/// @brief Resolve a combined type (T?!).
TypeAST* resolveCombinedType(CombinedTypeAST* type, SemaContext& ctx);

/// @brief Resolve a reference type (&T).
TypeAST* resolveRefType(RefTypeAST* type, SemaContext& ctx);

/// @brief Resolve a pointer type (*T).
TypeAST* resolvePtrType(PtrTypeAST* type, SemaContext& ctx);

/// @brief Resolve a function type.
TypeAST* resolveFuncType(FuncTypeAST* type, SemaContext& ctx);

/// @brief Resolve a trait reference to its declaration.
TraitDeclAST* resolveTraitRef(NamedTypeAST* ref, SemaContext& ctx);

/// @brief Resolve a call expression's callee to the FuncDeclAST it names.
FuncDeclAST* resolveCalleeOrError(ExprAST* callee, SemaContext& ctx);

// =============================================================================
// TYPE PREDICATES
// =============================================================================

// ─── Primitive Type Predicates ──────────────────────────────────────────

bool isBoolType(TypeAST* type);
bool isIntegerType(TypeAST* type);
bool isFloatType(TypeAST* type);
bool isNumericType(TypeAST* type);
bool isStringType(TypeAST* type);
bool isCharType(TypeAST* type);
bool isPrimitiveType(TypeAST* type);

// ─── Wrapper Type Predicates ────────────────────────────────────────────

bool isNullableType(TypeAST* type);
bool isFallibleType(TypeAST* type);
bool isReferenceType(TypeAST* type);
bool isPointerType(TypeAST* type);
bool isBorrowedType(TypeAST* type);

// ─── Named Type Predicates ──────────────────────────────────────────────

bool isStructType(TypeAST* type, SemaContext& ctx);
bool isEnumType(TypeAST* type, SemaContext& ctx);
bool isTraitType(TypeAST* type, SemaContext& ctx);
bool isGenericParamType(TypeAST* type, SemaContext& ctx);

// ─── Built-in Type Predicates ────────────────────────────────────────────

bool isArenaType(TypeAST* type); // Arena? type is not allowed, use Arena::empty()
bool isArenaDescriptorType(TypeAST* type);
bool isArenaBinding(VarDeclAST* decl);

/// Does NOT accept:
///   - Simd<T, N>? (nullable Simd is not allowed)
///   - Simd<T, N>! (fallible Simd is not allowed)
bool isSimdType(TypeAST* type);

/// Valid element types are:
///   - Signed integers: int8, int16, int32, int64
///   - Unsigned integers: uint8, uint16, uint32, uint64
///   - Floating point: float32, float64
bool isValidSimdElementType(TypeAST* type);
TypeAST* getSimdElementType(TypeAST* simdType);
uint64_t getSimdLaneCount(TypeAST* simdType);

// ─── Switch Type Checks ──────────────────────────────────────────────────

bool isValidSwitchType(TypeAST* type, SemaContext& ctx);
EnumDeclAST* getEnumDeclFromType(TypeAST* type, SemaContext& ctx);
bool isSwitchCaseCompatible(ExprAST* value, 
                             TypeAST* subjectType, 
                             SemaContext& ctx);

// ─── FFI Compatibility ──────────────────────────────────────────────────

bool isValidFFIType(TypeAST* type, SemaContext& ctx);

// ─── Numeric Helpers ─────────────────────────────────────────────────────

size_t getIntegerBitWidth(TypeAST* type);
TypeAST* getLargerIntegerType(TypeAST* a, TypeAST* b, SemaContext& ctx);
bool isIntegerPromotionSafe(TypeAST* target, TypeAST* source, SemaContext& ctx);

// ─── Type Unwrapping ─────────────────────────────────────────────────────

TypeAST* unwrapNullable(TypeAST* type);
TypeAST* unwrapFallible(TypeAST* type);

// =============================================================================
// TYPE EQUALITY & ASSIGNABILITY
// =============================================================================

/// @brief Compare two types for structural equality.
bool typesEqual(TypeAST* a, TypeAST* b);

/// @brief Check if a source type can be assigned to a target type.
bool isAssignable(TypeAST* target, TypeAST* source, SemaContext& ctx);

// =============================================================================
// SELF-REFERENCE DETECTION
// =============================================================================

/// @brief Check if a let initializer references the variable being declared.
void checkLetSelfReference(ExprAST* expr, InternedString varName, SemaContext& ctx);

/// @brief Check if a field type is a valid self-reference to the current struct.
bool isValidStructSelfReference(TypeAST* fieldType,
                                 StructDeclAST* currentStruct,
                                 SemaContext& ctx);

/// @brief Check if a field is accessible on a generic type.
bool isFieldAccessibleOnGenericType(TypeAST* genericType,
                                     InternedString fieldName,
                                     SemaContext& ctx);

/// @brief Get the type of a field on a generic type.
TypeAST* getFieldTypeOnGenericType(TypeAST* genericType,
                                    InternedString fieldName,
                                    SemaContext& ctx);

// =============================================================================
// SEMANTIC VALIDATION
// =============================================================================

// ─── Const Validation ────────────────────────────────────────────────────

/// @brief Validate that a const declaration has a definite type.
bool validateConstType(TypeAST* type,
                        InternedString name,
                        const char* kind,
                        SemaContext& ctx);

// ─── Trait Validation ────────────────────────────────────────────────────

bool validateTraitImplementation(StructDeclAST* structDecl,
                                  TraitDeclAST* traitDecl,
                                  SemaContext& ctx);

bool validateAllTraitImplementations(StructDeclAST* structDecl,
                                      SemaContext& ctx);

bool checkTraitFieldConflicts(StructDeclAST* structDecl,
                               SemaContext& ctx);

// ─── Generic Validation ──────────────────────────────────────────────────

bool validateGenericArguments(ArenaSpan<TypeAST*> args,
                               ArenaSpan<GenericParamDeclAST*> params,
                               BaseAST* useSite,
                               SemaContext& ctx);

bool validateGenericParameterUsage(ArenaSpan<GenericParamDeclAST*> params,
                                    const std::vector<TypeAST*>& types,
                                    BaseAST* useSite,
                                    SemaContext& ctx);

// ─── Downward Flow Rule ──────────────────────────────────────────────────

bool validateBorrowedContext(TypeAST* type, SemaContext& ctx);

// ─── FFI Validation ──────────────────────────────────────────────────────

bool validateForeignFunction(FuncDeclAST* decl,
                              AttributeAST* foreignAttr,
                              SemaContext& ctx);

// ─── Arena Validation ────────────────────────────────────────────────────

bool validateArenaInitializer(ExprAST* init, SemaContext& ctx);

// ─── Simd Validation ────────────────────────────────────────────────────

bool validateSimdType(SimdTypeAST* simdType, SemaContext& ctx);

} // namespace sema