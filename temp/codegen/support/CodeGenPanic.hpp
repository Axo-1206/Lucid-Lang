/// @file support/CodeGenPanic.hpp
/// @brief Panic and bounds checking utilities.
///
/// ─── Source Location Support ──────────────────────────────────────────────────
/// All panic functions now accept SourceLocation to provide file:line:column
/// information in panic messages. The file name is retrieved from
/// CodeGenContext::currentFile (set by CodeGen when processing each module).
///
/// ─── Panic Message Format ────────────────────────────────────────────────────
/// Messages include source location when available:
///   "main.luc:42:10: division by zero"
///
/// For unknown locations:
///   "division by zero"

#pragma once

#include "../context/CodeGenContext.hpp"
#include "codegen/runtime/RuntimeError.hpp"
#include "core/SourceLocation.hpp"
#include "core/ast/BaseAST.hpp"
#include <llvm/IR/Value.h>
#include <llvm/IR/BasicBlock.h>
#include <string>

namespace codegen {

/// @brief Build a panic message with source location and file name.
/// @param kind The runtime error kind.
/// @param ctx The code generation context (provides file name).
/// @param loc The source location (may be unknown).
/// @param additionalMessage Optional extra message.
/// @return Formatted panic message string.
std::string buildPanicMessage(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const SourceLocation& loc,
    const std::string& additionalMessage = ""
);

/// @brief Emit a panic call with source location.
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param message The panic message (optional - defaults to error kind message).
/// @param loc The source location (may be unknown).
void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const std::string& message,
    const SourceLocation& loc
);

/// @brief Emit a panic call with source location from an AST node.
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param node The AST node to extract location from.
void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    BaseAST* node
);

/// @brief Emit a panic call with source location and custom message.
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param message The panic message (optional - defaults to error kind message).
/// @param node The AST node to extract location from.
void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const std::string& message,
    BaseAST* node
);

/// @brief Emit a panic call with no source location (fallback).
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param message The panic message (optional - defaults to error kind message).
void emitPanic(
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    const std::string& message = ""
);

/// @brief Emit a zero check with optional fallback.
/// @param val The value to check (must be integer).
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on zero (for ??).
/// @param loc Source location for panic messages.
/// @return The checked value (if no fallback), or nullptr if fallback was taken.
llvm::Value* emitZeroCheck(
    llvm::Value* val,
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock = nullptr,
    const SourceLocation& loc = SourceLocation()
);

/// @brief Emit a zero check with source location from an AST node.
/// @param val The value to check (must be integer).
/// @param kind The runtime error kind.
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on zero (for ??).
/// @param node The AST node to extract location from.
/// @return The checked value (if no fallback), or nullptr if fallback was taken.
llvm::Value* emitZeroCheck(
    llvm::Value* val,
    RuntimeErrorKind kind,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    BaseAST* node
);

/// @brief Emit a bounds check with optional fallback.
/// @param index The index to check.
/// @param length The length to check against (0 <= index < length).
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on out-of-bounds.
/// @param loc Source location for panic messages.
/// @return The checked index (if no fallback), or nullptr if fallback was taken.
llvm::Value* emitBoundsCheck(
    llvm::Value* index,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock = nullptr,
    const SourceLocation& loc = SourceLocation()
);

/// @brief Emit a bounds check with source location from an AST node.
/// @param index The index to check.
/// @param length The length to check against (0 <= index < length).
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on out-of-bounds.
/// @param node The AST node to extract location from.
/// @return The checked index (if no fallback), or nullptr if fallback was taken.
llvm::Value* emitBoundsCheck(
    llvm::Value* index,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    BaseAST* node
);

/// @brief Emit a slice bounds check with optional fallback.
/// @param start The start index.
/// @param end The end index.
/// @param length The array length.
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on out-of-bounds.
/// @param loc Source location for panic messages.
/// @return A pair of checked start and end (if no fallback), or nullptr if fallback was taken.
std::pair<llvm::Value*, llvm::Value*> emitSliceBoundsCheck(
    llvm::Value* start,
    llvm::Value* end,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock = nullptr,
    const SourceLocation& loc = SourceLocation()
);

/// @brief Emit a slice bounds check with source location from an AST node.
/// @param start The start index.
/// @param end The end index.
/// @param length The array length.
/// @param ctx The code generation context.
/// @param fallbackBlock Optional block to branch to on out-of-bounds.
/// @param node The AST node to extract location from.
/// @return A pair of checked start and end (if no fallback), or nullptr if fallback was taken.
std::pair<llvm::Value*, llvm::Value*> emitSliceBoundsCheck(
    llvm::Value* start,
    llvm::Value* end,
    llvm::Value* length,
    CodeGenContext& ctx,
    llvm::BasicBlock* fallbackBlock,
    BaseAST* node
);

} // namespace codegen