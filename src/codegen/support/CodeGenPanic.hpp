/// @file support/CodeGenPanic.hpp
/// @brief Runtime panic and null check handling for code generation.

#pragma once

#include "../context/CodeGenContext.hpp"
#include <llvm/IR/Value.h>
#include <string>

namespace codegen {

/// @brief Emit a panic call with the given message.
/// @param message The panic message string.
/// @param ctx The code generation context.
/// @note This creates an unreachable instruction after the panic call.
void emitPanic(
    const std::string& message,
    CodeGenContext& ctx
);

/// @brief Emit a null check, panicking if the pointer is null.
/// @param ptr The pointer to check.
/// @param message The panic message if the pointer is null.
/// @param ctx The code generation context.
/// @return The original pointer (wrapped in a phi node for control flow).
llvm::Value* emitNullCheck(
    llvm::Value* ptr,
    const std::string& message,
    CodeGenContext& ctx
);

} // namespace codegen