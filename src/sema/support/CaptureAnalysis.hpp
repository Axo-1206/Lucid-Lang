/// @file CaptureAnalysis.hpp
/// @brief Analyzes closures to detect captured variables and escape analysis.
///
/// Capture analysis is performed during semantic analysis of anonymous functions
/// (closures) and nested function declarations. It walks the function body's AST
/// and identifies all IdentifierExprAST nodes that reference variables from outer
/// scopes, marking them as captures.
///
/// ## Key Responsibilities
///
/// 1. **Capture Detection**: Find all variables from outer scopes used inside a closure
/// 2. **Closure Detection**: Determine if a captured value is itself a closure
/// 3. **Mutation Analysis**: Detect if a captured variable is assigned to (by-reference vs by-value)
/// 4. **Escape Analysis**: Detect when a closure is returned (heap allocation needed)
///
/// ## Capture Rules (from Grammar.md)
///
/// | Type             | Can Capture?  | Why                                     |
/// | ---------------- | ------------  | --------------------------------------- |
/// | `&T` (reference) | ❌ No         | Downward Flow Rule - cannot flow upward |
/// | `[_]T` (slice)   | ❌ No         | Same as reference - borrowed view       |
/// | `Future<T>`      | ❌ No         | Linear type - can only be consumed once |
/// | `Thread<T>`      | ❌ No         | Linear type - can only be consumed once |
/// | Plain function   | ✅ Yes        | Function pointer, no environment        |
/// | Closure          | ✅ Yes        | Shared/refcounted - can be captured     |
/// | Owned values     | ✅ Yes        | Full copy (or by-reference if mutated)  |
/// | `*T` (raw ptr)   | ✅ Yes        | Sealed conduit - no lifetime guarantee  |
///
/// ## By-Reference vs By-Value
///
/// - **By-Reference**: Captured variable is mutated inside the closure body
/// - **By-Value**: Captured variable is only read (snapshot copy)
///
/// @related_files
///   - src/sema/rules/SemaExpr.cpp - resolveAnonFuncExpr calls analyzeCaptures
///   - src/sema/rules/SemaDecl.cpp - resolveFuncDecl calls analyzeCaptures
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

/// @brief Analyzes a nested function's body to detect captured variables.
///
/// This function walks the AST of the function body, finds all identifier
/// references, and determines which ones reference variables from outer scopes.
/// Nested functions (closureDepth > 0) that capture variables become closures.
///
/// @param func The function declaration to analyze.
/// @param ctx The semantic context (contains scope information).
void analyzeCaptures(FuncDeclAST* func, SemaContext& ctx);

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
///   // Nested function returned - marks as escaping
///   const counter () -> int = { return count += 1 };
///   return counter;
void markClosureIfEscaping(ExprAST* expr, SemaContext& ctx);

/// @brief Helper to determine if a value is a closure (has an environment).
///
/// Traverses the expression to find the underlying function declaration
/// and checks its hasClosure flag.
///
/// @param expr The expression representing a function value.
/// @param ctx The semantic context.
/// @return True if the value is a closure (has captured variables).
bool isClosureValue(ExprAST* expr, SemaContext& ctx);

} // namespace sema