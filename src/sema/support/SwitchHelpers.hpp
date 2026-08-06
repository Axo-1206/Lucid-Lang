/// @file SwitchHelpers.hpp
/// @brief Helpers for switch statement validation.

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
bool checkExhaustiveness(const SwitchStmtAST* stmt, 
                          const TypeAST* subjectType, 
                          SemaContext& ctx);

/// @brief Collect all enum variants covered by switch cases.
std::unordered_set<InternedString> collectCoveredVariants(
    const SwitchStmtAST* stmt, 
    SemaContext& ctx);

/// @brief Check if a case value is an enum variant.
bool isEnumVariantAccess(const ExprAST* value, SemaContext& ctx);

/// @brief Get the enum variant name from a case value.
InternedString getEnumVariantName(const ExprAST* value, SemaContext& ctx);

} // namespace switch_helpers
} // namespace sema