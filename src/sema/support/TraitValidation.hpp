/// @file TraitValidation.hpp
/// @brief Trait implementation validation helpers.
/// 
/// Provides functions for validating that structs correctly implement traits.
/// These are used during struct declaration analysis.

#pragma once

#include "core/ast/DeclAST.hpp"

namespace sema {

// Forward declarations
struct SemaContext;

// ─── Trait Validation ──────────────────────────────────────────────────────

/// @brief Validate that a struct implements a single trait.
/// 
/// Checks:
///   1. All trait fields exist in the struct
///   2. Field types match exactly (or are assignable)
///   3. Const-ness matches (trait const → struct const required)
/// 
/// @param structDecl The struct to validate.
/// @param traitDecl The trait to validate against.
/// @param ctx The semantic context.
/// @return true if the struct correctly implements the trait.
bool validateSingleTraitImplementation(const StructDeclAST* structDecl,
                                        const TraitDeclAST* traitDecl,
                                        SemaContext& ctx);

/// @brief Validate all trait implementations for a struct.
/// 
/// This is the main entry point called during struct analysis.
/// It validates each trait and registers successful implementations.
/// 
/// @param structDecl The struct to validate.
/// @param ctx The semantic context.
/// @return true if all trait implementations are valid.
bool validateAllTraitImplementations(const StructDeclAST* structDecl,
                                      SemaContext& ctx);

/// @brief Check for conflicting field names across traits.
/// 
/// If two traits require the same field name with different types,
/// or if two traits require the same field name with different const-ness,
/// this is a conflict and should be reported as an error.
/// 
/// @param structDecl The struct with traits to check.
/// @param ctx The semantic context.
/// @return true if no conflicts found.
bool checkTraitFieldConflicts(const StructDeclAST* structDecl,
                               SemaContext& ctx);

} // namespace sema