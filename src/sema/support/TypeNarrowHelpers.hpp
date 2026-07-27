/// @file TypeNarrowHelper.hpp
/// @brief Helper functions for type narrowing detection in if conditions.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "../context/SemaContext.hpp"

namespace sema {

/// @brief Extract all narrowing information from a condition expression.
/// 
/// Handles:
///   - Simple: x != nil, x == nil, x != err, x == err
///   - `or` at top level: x == nil or y == nil → both narrowings
///   - `and` at top level: No narrowing (unsound) → returns empty
///   - `not x`: Inverse narrowing (x is nil/false)
/// 
/// WARNING: Mixed operators (`==` and `!=` together) at the same logical level
///    are REJECTED and return no narrowing.
/// 
/// @param expr The condition expression.
/// @param ctx The semantic context.
/// @param outIsValidMixed Optional output parameter to detect mixed operators.
/// @return NarrowingInfo with all narrowings found, or empty if mixed/unsound.
NarrowingInfo extractNarrowingsFromCondition(const ExprAST* expr, SemaContext& ctx, 
                                               bool* outIsValidMixed = nullptr);

/// @brief Detect a single narrowing pattern in a binary expression.
/// 
/// Patterns detected:
///   - x == nil, x != nil
///   - x == err, x != err
///   - nil == x, nil != x (reverse order)
///   - err == x, err != x (reverse order)
/// 
/// @param binary The binary expression.
/// @param ctx The semantic context.
/// @return NarrowingInfo with the detected narrowing.
NarrowingInfo detectSingleNarrowing(const BinaryExprAST* binary, SemaContext& ctx);

/// @brief Detect if an identifier expression can be narrowed and add to info.
/// 
/// @param info The NarrowingInfo to add to.
/// @param id The identifier expression.
/// @param lit The literal (nil or err).
/// @param isEquality True if operator is ==, false if !=.
/// @param ctx The semantic context.
void detectIdentifierNarrowing(NarrowingInfo& info, const IdentifierExprAST* id, 
                                const LiteralExprAST* lit, bool isEquality, 
                                SemaContext& ctx);

/// @brief Get the inner type of a value declaration (unwrap nullable/fallible).
/// 
/// @param decl The value declaration.
/// @param ctx The semantic context.
/// @return The inner type, or nullptr if not found.
const TypeAST* getInnerType(const ValueDeclAST* decl, SemaContext& ctx);

/// @brief Detect narrowing pattern from a binary expression.
/// 
/// This is the main entry point for checkBinaryExpr to detect narrowing patterns.
/// It delegates to extractNarrowingsFromCondition but adds additional validation.
/// 
/// @param binary The binary expression to check.
/// @param ctx The semantic context.
/// @return NarrowingInfo with the detected narrowing, or empty if no pattern.
NarrowingInfo detectNarrowingPattern(const BinaryExprAST* binary, SemaContext& ctx);

} // namespace sema