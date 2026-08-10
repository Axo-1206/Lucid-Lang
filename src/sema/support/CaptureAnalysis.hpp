/// @file CaptureAnalysis.hpp
/// @brief Analyzes closures to detect captured variables from outer scopes.
///
/// Capture analysis is performed during semantic analysis of anonymous functions
/// (closures). It walks the function body's AST and identifies all IdentifierExprAST
/// nodes that reference variables from outer scopes, marking them as captures.
///
/// @related_files
///   - src/sema/rules/SemaExpr.cpp - resolveAnonFuncExpr calls analyzeCaptures
///   - src/codegen/CodeGenClosure.cpp - consumes CapturedVariable list
///
/// # How Capture Analysis Works
///
/// 1. Walk the AST of the closure body
/// 2. Find all IdentifierExprAST nodes
/// 3. For each identifier, determine if it references a variable from an outer scope
/// 4. Classify each capture as by-reference (mutable) or by-value (read-only)
/// 5. Store the list of captures on the AnonFuncExprAST
///
/// # Capture Rules (from LUCID_GRAMMAR.md)
///
/// 1. Variables captured mutably (`byReference = true`) → share one heap slot
/// 2. Variables captured read-only (`byReference = false`) → may be snapshot-copied
/// 3. Borrowed types (&T, [_]T) → NOT allowed to be captured (Downward Flow Rule)
/// 4. Linear types (Future<T>, Thread<T>) → NOT allowed to be captured
/// 5. Module members → NOT captured (they're global, program-lifetime)
/// 6. The closure's OWN parameter list → NOT a capture

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

} // namespace sema