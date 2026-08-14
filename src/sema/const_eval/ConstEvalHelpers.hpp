/// @file const_eval/ConstEvalHelpers.hpp
/// @brief Internal helpers for const evaluation - not part of public API.

#pragma once

#include "ConstEvaluator.hpp"
#include "sema/context/SemaContext.hpp"
#include "sema/types/SemaCompare.hpp"

namespace sema {

// ─── Type Helpers ──────────────────────────────────────────────────────

/// @brief Get the type of a constant value.
/// Set by const evaluator, read by code generator.
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
                                     BaseAST* node,
                                     TypeAST* targetType);

// ─── Dependency Helpers ─────────────────────────────────────────────────

/// @brief Collect dependencies from an expression.
/// Collected by const evaluator, used for topological sorting.
void collectDeps(SemaContext& ctx, ExprAST* expr, 
                 std::vector<DeclAST*>& deps);

/// @brief Collect dependencies from a statement.
void collectDepsFromStmt(SemaContext& ctx, StmtAST* stmt,
                         std::vector<DeclAST*>& deps);

/// @brief Topological sort of const declarations.
/// Computed by const evaluator, used to determine evaluation order.
std::vector<DeclAST*> topologicalSort(SemaContext& ctx,
                                      const std::unordered_map<DeclAST*, std::vector<DeclAST*>>& deps);

} // namespace sema