/// @file support/CodeGenPanic.hpp
/// @brief Panic and bounds checking utilities.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "codegen/runtime/RuntimeError.hpp"
#include <llvm/IR/Value.h>
#include <llvm/IR/BasicBlock.h>

namespace codegen {

/// @brief Emit a zero check with optional fallback.
/// @param val The value to check (must be integer).
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on zero (for ??).
/// @return The checked value (if no fallback), or nullptr if fallback was taken.
llvm::Value* emitZeroCheck(
    llvm::Value* val,
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock = nullptr
);

/// @brief Emit a bounds check with optional fallback.
/// @param index The index to check.
/// @param length The length to check against (0 <= index < length).
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on out-of-bounds.
/// @return The checked index (if no fallback), or nullptr if fallback was taken.
llvm::Value* emitBoundsCheck(
    llvm::Value* index,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock = nullptr
);

/// @brief Emit a slice bounds check with optional fallback.
/// @param start The start index.
/// @param end The end index.
/// @param length The array length.
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on out-of-bounds.
/// @return A pair of checked start and end (if no fallback), or nullptr if fallback was taken.
std::pair<llvm::Value*, llvm::Value*> emitSliceBoundsCheck(
    llvm::Value* start,
    llvm::Value* end,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock = nullptr
);

/// @brief Emit a panic call.
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param message The panic message (optional).
void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const std::string& message = ""
);

/// @brief Build a panic message with location info.
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param loc The source location.
/// @return The panic message string.
std::string buildPanicMessage(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    SourceLocation loc
);

} // namespace codegen