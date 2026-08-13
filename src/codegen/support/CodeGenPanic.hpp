/// @file support/CodeGenPanic.hpp
/// @brief Runtime panic and null check handling for code generation.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "RuntimeError.hpp"
#include <llvm/IR/Value.h>
#include <string>

namespace codegen {

/// @brief Emit a panic call with the given runtime error.
void emitPanic(RuntimeErrorKind kind, CodeGenContext& ctx);

/// @brief Emit a panic call with a custom message.
void emitPanic(const std::string& message, CodeGenContext& ctx);

/// @brief Emit a null check, panicking if the pointer is null.
llvm::Value* emitNullCheck(llvm::Value* ptr, CodeGenContext& ctx);

/// @brief Emit a bounds check for array indexing.
llvm::Value* emitBoundsCheck(
    llvm::Value* index,
    llvm::Value* length,
    CodeGenContext& ctx
);

/// @brief Emit a bounds check for slice bounds.
std::pair<llvm::Value*, llvm::Value*> emitSliceBoundsCheck(
    llvm::Value* start,
    llvm::Value* end,
    llvm::Value* length,
    CodeGenContext& ctx
);

/// @brief Emit a zero check for division/modulo.
llvm::Value* emitZeroCheck(llvm::Value* divisor, RuntimeErrorKind kind, CodeGenContext& ctx);

} // namespace codegen