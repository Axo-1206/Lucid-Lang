/// @file registry/IntrinsicValidator.hpp
/// @brief Pure validation functions for intrinsics - no state.

#pragma once

#include "../context/SemaContext.hpp"
#include "../types/SemaType.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include <string>

namespace sema {

// ─── Main Validation Functions ────────────────────────────────────────────

/// @brief Validate an intrinsic call.
/// @param expr The intrinsic call expression.
/// @param ctx The semantic context.
/// @return true if the intrinsic is valid.
bool validateIntrinsicCall(IntrinsicCallExprAST* expr, SemaContext& ctx);

/// @brief Validate argument count for an intrinsic.
/// @param name The intrinsic name.
/// @param count The number of arguments.
/// @param ctx The semantic context.
/// @return true if the argument count is valid.
bool validateIntrinsicArgCount(InternedString name, size_t count, SemaContext& ctx);

/// @brief Get the return type of an intrinsic call.
/// @param expr The intrinsic call expression.
/// @param targetType The target type (for context-dependent intrinsics).
/// @param ctx The semantic context.
/// @return The return type, or nullptr for void.
TypeAST* getIntrinsicReturnType(IntrinsicCallExprAST* expr,
                                       TypeAST* targetType,
                                       SemaContext& ctx);

/// @brief Get the value state of an intrinsic call.
/// @param expr The intrinsic call expression.
/// @param ctx The semantic context.
/// @return The value state.
ValueState getIntrinsicValueState(IntrinsicCallExprAST* expr, SemaContext& ctx);

/// @brief Check if an intrinsic returns void (no value).
/// @param name The intrinsic name.
/// @param ctx The semantic context.
/// @return true if the intrinsic returns void.
bool isIntrinsicVoid(InternedString name, SemaContext& ctx);

// ─── Individual Intrinsic Validators ──────────────────────────────────────

bool validateFloatingPoint(IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateMemoryOp(IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateFence(IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateStringOp(IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validatePointerOp(IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateScopeExit(IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateAtomicOp(IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateSIMD(IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateMemoryManagement(IntrinsicCallExprAST* expr, SemaContext& ctx);

/// @brief Validate #tostr intrinsic.
/// @param expr The intrinsic call expression.
/// @param ctx The semantic context.
/// @return true if the #tostr call is valid.
bool validateTostr(IntrinsicCallExprAST* expr, SemaContext& ctx);

} // namespace sema