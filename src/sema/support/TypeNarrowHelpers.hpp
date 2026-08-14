/// @file TypeNarrowHelpers.hpp
/// @brief Helper functions for type narrowing detection in if conditions.
/// 
/// Type narrowing allows the compiler to refine variable types based on
/// conditional checks. For example:
///   if x != nil { ... }  // x is non-nullable inside the block
///   if x == nil { return }  // x is non-nullable after the check
/// 
/// @design_decision Uses existing semantic infrastructure
///   - SemaCompare for type predicates (isNullableType, isFallibleType)
///   - SemaLookup for name resolution (lookupValue)
///   - SemaResolve for type resolution (getInnerType)
///   - DiagnosticEngine for error reporting

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

// ─── Main Entry Points ────────────────────────────────────────────────────

/// @brief Extract all narrowing information from a condition expression.
/// 
/// Handles:
///   - Simple: x != nil, x == nil, x != err, x == err
///   - `or` at top level: x == nil or y == nil → both narrowings
///   - `and` at top level: No narrowing (unsound) → returns empty
///   - `not x`: Inverse narrowing (x is nil/false)
/// 
/// @param expr The condition expression.
/// @param ctx The semantic context.
/// @param outIsValidMixed Optional output parameter to detect mixed operators.
/// @return NarrowingInfo with all narrowings found, or empty if mixed/unsound.
NarrowingInfo extractNarrowingsFromCondition(ExprAST* expr, SemaContext& ctx, 
                                               bool* outIsValidMixed = nullptr);

/// @brief Detect narrowing pattern from a binary expression.
/// 
/// This is the main entry point for checkBinaryExpr to detect narrowing patterns.
/// It delegates to extractNarrowingsFromCondition but adds additional validation.
/// 
/// @param binary The binary expression to check.
/// @param ctx The semantic context.
/// @return NarrowingInfo with the detected narrowing, or empty if no pattern.
NarrowingInfo detectNarrowingPattern(BinaryExprAST* binary, SemaContext& ctx);

// ─── Internal Helpers (exposed for testing) ─────────────────────────────

/// @brief Detect a single narrowing pattern in a binary expression.
/// 
/// Patterns detected:
///   - x == nil, x != nil
///   - x == err, x != err
///   - nil == x, nil != x (reverse order)
///   - err == x, err != x (reverse order)
NarrowingInfo detectSingleNarrowing(BinaryExprAST* binary, SemaContext& ctx);

/// @brief Detect if an identifier expression can be narrowed and add to info.
void detectIdentifierNarrowing(NarrowingInfo& info, IdentifierExprAST* id, 
                                const LiteralExprAST* lit, bool isEquality, 
                                SemaContext& ctx);

/// @brief Get the inner type of a value declaration (unwrap nullable/fallible).
TypeAST* getInnerType(ValueDeclAST* decl, SemaContext& ctx);

} // namespace sema