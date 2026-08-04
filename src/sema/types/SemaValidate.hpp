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
bool validateConstType(const TypeAST* type,
                        InternedString name,
                        const char* kind,
                        SemaContext& ctx);

/// @brief Validate that a const declaration has an initializer.
/// 
/// Const declarations must always have an initializer because they cannot
/// be assigned later.
/// 
/// @param hasInit True if the declaration has an initializer.
/// @param name The name of the declaration (for error messages).
/// @param kind The kind of declaration (for error messages).
/// @param ctx The semantic context.
/// @return true if the declaration has an initializer.
bool validateConstInitializer(bool hasInit,
                               InternedString name,
                               const char* kind,
                               SemaContext& ctx);

// ─── Trait Validation ────────────────────────────────────────────────────

/// @brief Validate that a struct implements all fields of a single trait.
bool validateTraitImplementation(const StructDeclAST* structDecl,
                                  const TraitDeclAST* traitDecl,
                                  SemaContext& ctx);

/// @brief Validate all trait implementations for a struct.
bool validateAllTraitImplementations(const StructDeclAST* structDecl,
                                      SemaContext& ctx);

/// @brief Check for conflicting field names across multiple traits.
bool checkTraitFieldConflicts(const StructDeclAST* structDecl,
                               SemaContext& ctx);

// ─── Generic Validation ──────────────────────────────────────────────────

/// @brief Validate generic arguments against parameters.
bool validateGenericArguments(ArenaSpan<TypePtr> args,
                               ArenaSpan<GenericParamDeclPtr> params,
                               const BaseAST* useSite,
                               SemaContext& ctx);

/// @brief Validate that all generic parameters are used in the declaration.
bool validateGenericParameterUsage(ArenaSpan<GenericParamDeclPtr> params,
                                    const std::vector<const TypeAST*>& types,
                                    const BaseAST* useSite,
                                    SemaContext& ctx);

/// @brief Check if a field is accessible on a generic type.
bool isFieldAccessibleOnGenericType(const TypeAST* genericType,
                                    InternedString fieldName,
                                    SemaContext& ctx);

/// @brief Get the type of a field on a generic type.
const TypeAST* getFieldTypeOnGenericType(const TypeAST* genericType,
                                         InternedString fieldName,
                                         SemaContext& ctx);

// ─── Downward Flow Rule ──────────────────────────────────────────────────

/// @brief Validate that a reference type appears in a valid context.
bool validateRefContext(const RefTypeAST* type, SemaContext& ctx);

} // namespace sema