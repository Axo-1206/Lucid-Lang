/// @file CaptureAnalysis.hpp
/// @brief Analyzes closures to detect captured variables and escape analysis.
///
/// Capture analysis is performed during semantic analysis of anonymous functions
/// (closures). It walks the function body's AST and identifies all IdentifierExprAST
/// nodes that reference variables from outer scopes, marking them as captures.
///
/// Escape analysis detects when a closure is returned from a function or stored
/// in a way that outlives the function call, which affects allocation strategy.
///
/// @related_files
///   - src/sema/rules/SemaExpr.cpp - resolveAnonFuncExpr calls analyzeCaptures
///   - src/sema/rules/SemaStmt.cpp - resolveReturnStmt calls markClosureIfEscaping
///   - src/codegen/CodeGenClosure.cpp - consumes CapturedVariable list

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "../context/SemaContext.hpp"

#include <vector>
#include <unordered_set>

namespace sema {

/// @brief Analyzes a closure's body to detect captured variables.
///
/// This function walks the AST of the closure body, finds all identifier
/// references, and determines which ones reference variables from outer scopes.
///
/// @param expr The anonymous function expression to analyze.
/// @param ctx The semantic context (contains scope information).
void analyzeCaptures(AnonFuncExprAST* expr, SemaContext& ctx);

/// @brief Detects if a returned expression contains a closure that escapes.
///
/// A closure is considered "escaping" if it's created locally and returned to
/// the caller. Such closures must be heap-allocated because they outlive the
/// function call.
///
/// Static closures (module members) do NOT need to be marked as escaping
/// because they live for the entire program lifetime.
///
/// @param expr The returned expression.
/// @param ctx The semantic context.
///
/// @example
///   // Direct closure return - marks as escaping
///   return (n int) -> int { return n + 1 };
///
///   // Module member - not marked as escaping
///   return module:myClosure;
///
///   // Local variable holding a closure - marks as escaping
///   let c = (n int) -> int { return n + 1 };
///   return c;
void markClosureIfEscaping(const ExprAST* expr, SemaContext& ctx);

} // namespace sema