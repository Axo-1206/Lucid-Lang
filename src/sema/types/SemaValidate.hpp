/// @file SemaValidate.hpp
/// @brief Semantic validation rules for declarations and constructs.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ArenaSpan.hpp"
#include "../context/SemaContext.hpp"

#include <vector>

namespace sema {

// ─── Const Validation ────────────────────────────────────────────────────

/// @brief Validate that a const declaration has a definite (non-nullable, non-fallible) type.
/// 
/// Const declarations cannot be nullable or fallible because they must have
/// a definite value at compile time.
/// 
/// @param type The type to validate.
/// @param name The name of the declaration (for error messages).
/// @param kind The kind of declaration (for error messages).
/// @param ctx The semantic context.
/// @return true if the type is valid for a const declaration.
/// 
/// @example
///   validateConstType(type, "x", "variable", ctx);
///   validateConstType(type, "field", "struct field", ctx);
bool validateConstType(TypeAST* type,
                        InternedString name,
                        const char* kind,
                        SemaContext& ctx);

// ─── Trait Validation ────────────────────────────────────────────────────

/// @brief Validate that a struct implements all fields of a single trait.
bool validateTraitImplementation(StructDeclAST* structDecl,
                                  TraitDeclAST* traitDecl,
                                  SemaContext& ctx);

/// @brief Validate all trait implementations for a struct.
bool validateAllTraitImplementations(StructDeclAST* structDecl,
                                      SemaContext& ctx);

/// @brief Check for conflicting field names across multiple traits.
bool checkTraitFieldConflicts(StructDeclAST* structDecl,
                               SemaContext& ctx);

// ─── Generic Validation ──────────────────────────────────────────────────

/// @brief Validate generic arguments against parameters.
bool validateGenericArguments(ArenaSpan<TypeAST*> args,
                               ArenaSpan<GenericParamDeclAST*> params,
                               BaseAST* useSite,
                               SemaContext& ctx);

/// @brief Validate that all generic parameters are used in the declaration.
bool validateGenericParameterUsage(ArenaSpan<GenericParamDeclAST*> params,
                                    const std::vector<TypeAST*>& types,
                                    BaseAST* useSite,
                                    SemaContext& ctx);

// ─── Downward Flow Rule ──────────────────────────────────────────────────

/// @brief Validate that a borrowed type appears in a valid context.
/// 
/// Borrowed types are:
///   - &T (references)
///   - [_]T (slices)
/// 
/// The Downward Flow Rule forbids borrowed types from:
///   1. Struct fields
///   2. Array/Slice elements
///   3. Function returns
///   4. Closure captures
/// 
/// @param type The borrowed type to validate.
/// @param ctx The semantic context.
/// @return true if the borrowed type is in a valid context.
/// 
/// @deprecated Use validateBorrowedContext from SemaResolve.hpp instead.
bool validateRefContext(RefTypeAST* type, SemaContext& ctx);

// ─── FFI Validation ──────────────────────────────────────────────────────

/// @brief Validate a foreign function declaration.
/// 
/// Checks:
///   1. ABI string must be "C"
///   2. All parameter types must be FFI-compatible
///   3. Return type must be FFI-compatible
///   4. Function must have no body
/// 
/// @param decl The function declaration.
/// @param foreignAttr The @[foreign] attribute.
/// @param ctx The semantic context.
/// @return true if valid, false otherwise.
bool validateForeignFunction(FuncDeclAST* decl,
                              AttributeAST* foreignAttr,
                              SemaContext& ctx);

} // namespace sema