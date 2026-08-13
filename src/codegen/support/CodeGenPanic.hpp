/// @file support/CodeGenPanic.hpp
/// @brief Runtime panic and null check handling for code generation.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides helpers for emitting runtime error handling code:
///   - Panic: Abort execution with a message (like Rust's panic!)
///   - Null Check: Assert a pointer is non-null, panic if it's null
///
/// ─── Why a Separate File? ──────────────────────────────────────────────────
/// Runtime error handling is a cross-cutting concern that appears in many
/// CodeGen contexts:
///   - Array bounds checks (IndexExpr, SliceExpr)
///   - Nullable type unwrapping (NullCoalesceExpr)
///   - Reference validation (#toRef intrinsic)
///   - Assertion failures
///
/// Centralizing panic logic ensures:
///   1. Consistent error messages across all CodeGen
///   2. Single place to manage the runtime panic function declaration
///   3. Proper control flow (panic is always followed by unreachable)
///   4. Easy to swap to different panic implementations (e.g., C++ exceptions)
///
/// ─── Runtime Integration ──────────────────────────────────────────────────
/// Panic calls invoke `__lucid_panic`, a runtime function declared in the
/// Lucid runtime library. This function:
///   1. Prints the panic message to stderr
///   2. Prints a stack trace (if available)
///   3. Terminates the process (abort)
///
/// The runtime function is lazily declared the first time `emitPanic()` is
/// called, using the `CodeGenContext::getRuntimeFunction()` mechanism.
///
/// ─── Control Flow Impact ──────────────────────────────────────────────────
/// Both helpers create unreachable instructions after the panic call:
///   - `emitPanic()` creates `call panic` + `unreachable`
///   - `emitNullCheck()` creates a branch: null → panic block → unreachable
///
/// This is critical for LLVM's correctness: a function that panics on some
/// paths must still have a valid CFG on all paths. The unreachable instruction
/// tells LLVM that the panic path never returns, enabling optimizations.
///
/// ─── Usage Example ─────────────────────────────────────────────────────────
/// @code
/// // Panic with a message
/// emitPanic("array index out of bounds", ctx);
///
/// // Null check with custom message
/// llvm::Value* ptr = ...;
/// llvm::Value* checked = emitNullCheck(ptr, "dereferenced null pointer", ctx);
/// // 'checked' is the same as 'ptr' but with a null-check inserted
/// @endcode
///
/// ─── Null Check Control Flow Diagram ──────────────────────────────────────
/// @code
///           ┌─────────────────┐
///           │   ptr != null?  │
///           └────────┬────────┘
///                    │
///         ┌─────────┴─────────┐
///         │                   │
///         ▼                   ▼
///  ┌─────────────┐    ┌─────────────┐
///  │   pass      │    │   fail      │
///  │  (use ptr)  │    │  panic(msg) │
///  └──────┬──────┘    └──────┬──────┘
///         │                  │
///         └────────┬─────────┘
///                  ▼
///           ┌─────────────┐
///           │   merge     │
///           │  (phi: ptr) │
///           └─────────────┘
/// @endcode

#pragma once

#include "../context/CodeGenContext.hpp"
#include <llvm/IR/Value.h>
#include <string>

namespace codegen {

/// @brief Emit a panic call with the given message.
/// @param message The panic message string.
/// @param ctx The code generation context.
/// @note This creates an unreachable instruction after the panic call.
///
/// @details The panic message is stored as a global string constant to avoid
///          copying it onto the stack. This is more efficient and ensures the
///          message is available even if the stack is corrupted.
///
/// @warning After calling this, the current basic block is terminated with
///          an `unreachable` instruction. The caller must NOT emit further
///          instructions in the same block.
void emitPanic(
    const std::string& message,
    CodeGenContext& ctx
);

/// @brief Emit a null check, panicking if the pointer is null.
/// @param ptr The pointer to check.
/// @param message The panic message if the pointer is null.
/// @param ctx The code generation context.
/// @return The original pointer (wrapped in a phi node for control flow).
///
/// @details This function emits a conditional branch:
///           - If ptr == null → panic with message
///           - If ptr != null → continue with ptr
///           The result is a phi node that returns the pointer on the success
///           path and null on the failure path (which is unreachable).
///
/// @note This is used for `#toRef` assertions and nullable type unwrapping.
///       The returned value should be used as the checked pointer.
///
/// @example
///   // Check that a raw pointer is valid before converting to a reference
///   llvm::Value* rawPtr = ...;
///   llvm::Value* checked = emitNullCheck(rawPtr, "null pointer in #toRef", ctx);
///   llvm::Value* ref = ctx.builder.CreatePointerCast(checked, refType);
llvm::Value* emitNullCheck(
    llvm::Value* ptr,
    const std::string& message,
    CodeGenContext& ctx
);

} // namespace codegen