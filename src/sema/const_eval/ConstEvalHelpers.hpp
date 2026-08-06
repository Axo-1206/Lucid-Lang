/// @file const_eval/ConstEvalHelpers.hpp
/// @brief Internal helpers for const evaluation - not part of public API.

#pragma once

#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"

namespace sema {

// ─── Type Helpers ──────────────────────────────────────────────────────

/// @brief Get the type of a constant value.
TypeAST* getConstantType(SemaContext& ctx, const ConstantValue& val);

/// @brief Convert a constant value to double (for numeric operations).
double toDouble(const ConstantValue& v);

/// @brief Check if two constant values are both numeric.
bool bothNumeric(const ConstantValue& a, const ConstantValue& b);

// ─── Error Helpers ──────────────────────────────────────────────────────

/// @brief Handle arithmetic errors (division by zero, etc.)
ConstantValue handleArithmeticError(SemaContext& ctx, 
                                     const char* op, 
                                     const std::string& reason,
                                     const BaseAST* node,
                                     const TypeAST* targetType);

// ─── Dependency Helpers ─────────────────────────────────────────────────

/// @brief Collect dependencies from an expression.
void collectDeps(SemaContext& ctx, const ExprAST* expr, 
                 std::vector<const DeclAST*>& deps);

/// @brief Collect dependencies from a statement.
void collectDepsFromStmt(SemaContext& ctx, const StmtAST* stmt,
                         std::vector<const DeclAST*>& deps);

/// @brief Topological sort of const declarations.
std::vector<const DeclAST*> topologicalSort(SemaContext& ctx,
                                             const std::unordered_map<const DeclAST*, std::vector<const DeclAST*>>& deps);

} // namespace sema