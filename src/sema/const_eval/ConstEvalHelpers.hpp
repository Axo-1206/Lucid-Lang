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

// ─── Error Helpers ──────────────────────────────────────────────────────

/// @brief Handle arithmetic errors (division by zero, etc.)
ConstantValue handleArithmeticError(SemaContext& ctx, 
                                     const char* op, 
                                     const std::string& reason,
                                     BaseAST* node,
                                     TypeAST* targetType);

// ─── Dependency Helpers ─────────────────────────────────────────────────

/// @brief Collect dependencies from an expression.
void collectDeps(SemaContext& ctx, ExprAST* expr, 
                 std::vector<DeclAST*>& deps);

/// @brief Collect dependencies from a statement.
void collectDepsFromStmt(SemaContext& ctx, StmtAST* stmt,
                         std::vector<DeclAST*>& deps);

/// @brief Topological sort of const declarations.
std::vector<DeclAST*> topologicalSort(SemaContext& ctx,
                                      const std::unordered_map<DeclAST*, std::vector<DeclAST*>>& deps);

} // namespace sema