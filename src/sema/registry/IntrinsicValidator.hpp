/// @file registry/IntrinsicValidator.hpp
/// @brief Pure validation functions for intrinsics - no state.

#pragma once

#include "../context/SemaContext.hpp"
#include "../types/SemaCompare.hpp"
#include "../types/SemaResolve.hpp"
#include "core/diagnostics/Diagnostic.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include <string>

namespace sema {

// ─── Main Validation Functions ────────────────────────────────────────────

/// @brief Validate an intrinsic call.
/// @param expr The intrinsic call expression.
/// @param ctx The semantic context.
/// @return true if the intrinsic is valid.
bool validateIntrinsicCall(const IntrinsicCallExprAST* expr, SemaContext& ctx);

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
const TypeAST* getIntrinsicReturnType(const IntrinsicCallExprAST* expr,
                                       const TypeAST* targetType,
                                       SemaContext& ctx);

/// @brief Get the value state of an intrinsic call.
/// @param expr The intrinsic call expression.
/// @param ctx The semantic context.
/// @return The value state.
ValueState getIntrinsicValueState(const IntrinsicCallExprAST* expr, SemaContext& ctx);

/// @brief Check if an intrinsic returns void (no value).
/// @param name The intrinsic name.
/// @param ctx The semantic context.
/// @return true if the intrinsic returns void.
bool isIntrinsicVoid(InternedString name, SemaContext& ctx);

// ─── Individual Intrinsic Validators ──────────────────────────────────────

bool validateFloatingPoint(const IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateMemoryOp(const IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateFence(const IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateStringOp(const IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validatePointerOp(const IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateScopeExit(const IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateAtomicOp(const IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateSIMD(const IntrinsicCallExprAST* expr, SemaContext& ctx);
bool validateMemoryManagement(const IntrinsicCallExprAST* expr, SemaContext& ctx);

// ─── Argument Type Validators ─────────────────────────────────────────────

bool validatePtrArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx);
bool validateNumericArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx);
bool validateIntArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx);
bool validateStringArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx);
bool validateBoolArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx);
bool validateRefArg(const ExprAST* arg, const std::string& argName, SemaContext& ctx);

} // namespace sema