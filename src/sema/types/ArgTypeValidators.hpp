/// @file registry/ArgumentTypeValidators.hpp
/// @brief Type validation utilities for intrinsic arguments.

#pragma once

#include "../context/SemaContext.hpp"
#include "../types/SemaCompare.hpp"
#include <string>

namespace sema {

// ─── Argument Type Validators ─────────────────────────────────────────────

/// @brief Validate that an argument is a pointer type.
/// @param arg The argument expression.
/// @param argName The name of the argument (for error messages).
/// @param ctx The semantic context.
/// @return true if the argument is a pointer type.
bool validatePtrArg(ExprAST* arg, const std::string& argName, SemaContext& ctx);

/// @brief Validate that an argument is a numeric type.
/// @param arg The argument expression.
/// @param argName The name of the argument (for error messages).
/// @param ctx The semantic context.
/// @return true if the argument is a numeric type.
bool validateNumericArg(ExprAST* arg, const std::string& argName, SemaContext& ctx);

/// @brief Validate that an argument is an integer type.
/// @param arg The argument expression.
/// @param argName The name of the argument (for error messages).
/// @param ctx The semantic context.
/// @return true if the argument is an integer type.
bool validateIntArg(ExprAST* arg, const std::string& argName, SemaContext& ctx);

/// @brief Validate that an argument is a string type.
/// @param arg The argument expression.
/// @param argName The name of the argument (for error messages).
/// @param ctx The semantic context.
/// @return true if the argument is a string type.
bool validateStringArg(ExprAST* arg, const std::string& argName, SemaContext& ctx);

/// @brief Validate that an argument is a boolean type.
/// @param arg The argument expression.
/// @param argName The name of the argument (for error messages).
/// @param ctx The semantic context.
/// @return true if the argument is a boolean type.
bool validateBoolArg(ExprAST* arg, const std::string& argName, SemaContext& ctx);

/// @brief Validate that an argument is a reference type.
/// @param arg The argument expression.
/// @param argName The name of the argument (for error messages).
/// @param ctx The semantic context.
/// @return true if the argument is a reference type.
bool validateRefArg(ExprAST* arg, const std::string& argName, SemaContext& ctx);

} // namespace sema