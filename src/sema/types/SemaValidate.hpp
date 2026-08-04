/// @file SemaValidate.hpp
/// @brief Semantic validation rules for declarations and constructs.
/// 
/// These functions check semantic rules that go beyond type resolution:
/// - Trait implementation validation
/// - Generic constraint validation
/// - Const field validation
/// - Downward Flow Rule (reference types)
/// 
/// @validation_rules
///   1. Trait implementations must provide all required fields
///   2. Generic arguments must satisfy constraints
///   3. Const fields cannot be nullable or fallible
///   4. Reference types cannot be stored (Downward Flow)

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/ArenaSpan.hpp"
#include "../context/SemaContext.hpp"

#include <vector>

namespace sema {

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

// ─── Const Field Validation ──────────────────────────────────────────────

/// @brief Validate that a trait field's type is not nullable or fallible.
bool validateTraitFieldType(const TypeAST* type, SemaContext& ctx);

// ─── Downward Flow Rule ──────────────────────────────────────────────────

/// @brief Validate that a reference type appears in a valid context.
bool validateRefContext(const RefTypeAST* type, SemaContext& ctx);

} // namespace sema