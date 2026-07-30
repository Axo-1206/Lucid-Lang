/// @file SwitchHelpers.hpp
/// @brief Helpers for switch statement validation.
/// 
/// Provides functions for validating switch statements, checking
/// exhaustiveness, and collecting enum variants.

#pragma once

#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../context/SemaContext.hpp"

#include <unordered_set>

namespace sema {
namespace switch_helpers {

// ─── Exhaustiveness Checking ──────────────────────────────────────────────

/// @brief Check exhaustiveness of enum switch cases.
/// 
/// For enum types with no default clause, all variants must be covered.
/// 
/// @param stmt The switch statement.
/// @param subjectType The switch subject type (must be an enum).
/// @param ctx The semantic context.
/// @return true if all variants are covered (or default exists).
bool checkExhaustiveness(const SwitchStmtAST* stmt, 
                          const TypeAST* subjectType, 
                          SemaContext& ctx);

/// @brief Collect all enum variants covered by switch cases.
/// 
/// @param stmt The switch statement.
/// @param ctx The semantic context.
/// @return A set of covered variant names.
std::unordered_set<InternedString> collectCoveredVariants(
    const SwitchStmtAST* stmt, 
    SemaContext& ctx);

/// @brief Check if a case value is an enum variant.
/// 
/// @param value The case value expression.
/// @param ctx The semantic context.
/// @return true if the value is an enum variant access.
bool isEnumVariantAccess(const ExprAST* value, SemaContext& ctx);

/// @brief Get the enum variant name from a case value.
/// 
/// @param value The case value expression (must be an enum variant access).
/// @param ctx The semantic context.
/// @return The variant name, or empty if not an enum variant.
InternedString getEnumVariantName(const ExprAST* value, SemaContext& ctx);

/// @brief Get the enum declaration from a case value.
/// 
/// @param value The case value expression (must be an enum variant access).
/// @param ctx The semantic context.
/// @return The EnumDeclAST, or nullptr if not an enum variant.
const EnumDeclAST* getEnumDeclFromVariantAccess(const ExprAST* value, SemaContext& ctx);

// ─── Case Value Validation ────────────────────────────────────────────────

/// @brief Validate a switch case value against the subject type.
/// 
/// @param value The case value expression.
/// @param subjectType The switch subject type.
/// @param ctx The semantic context.
/// @return true if the value is valid for the subject type.
bool validateCaseValue(const ExprAST* value, 
                        const TypeAST* subjectType, 
                        SemaContext& ctx);

} // namespace switch_helpers
} // namespace sema