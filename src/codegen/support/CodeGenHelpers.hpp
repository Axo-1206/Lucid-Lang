/// @file CodeGenHelpers.hpp
/// @brief Helper functions for code generation - allocas, blocks, loads, panic.

#pragma once

#include "../context/CodeGenContext.hpp"
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <string>

namespace codegen {

// ─── Forward Declaration ──────────────────────────────────────────────────
// getType is declared in CodeGenType.hpp

/// @brief Get the LLVM type for a Lucid type annotation.
/// @param ctx The code generation context.
/// @param type The Lucid type annotation.
/// @return The LLVM type, or nullptr if the type cannot be mapped.
llvm::Type* getType(CodeGenContext& ctx, const TypeAST* type);

// ─── Alloca Creation ──────────────────────────────────────────────────────

/// @brief Create an alloca in the current function's entry block.
/// @param name The variable name.
/// @param type The LLVM type to allocate.
/// @param ctx The code generation context.
/// @return The alloca instruction, or nullptr if no current function.
llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
);

/// @brief Create a named basic block in the current function.
/// @param name The block name.
/// @param ctx The code generation context.
/// @return The new basic block, or nullptr if no current function.
llvm::BasicBlock* createBlock(
    const std::string& name,
    CodeGenContext& ctx
);

// ─── Load Helpers ─────────────────────────────────────────────────────────

/// @brief Load a value from a pointer with explicit element type.
/// @param value The pointer value to load from.
/// @param elemType The LLVM type of the element to load.
/// @param ctx The code generation context.
/// @return The loaded value, or the original if not a pointer.
llvm::Value* loadIfNeeded(
    llvm::Value* value,
    llvm::Type* elemType,
    CodeGenContext& ctx
);

/// @brief Convenience overload - attempts to load if isLValue is true.
/// @note With opaque pointers, the element type cannot be inferred from the pointer.
///       Prefer the version with explicit elemType.
/// @param value The value (might be a pointer).
/// @param isLValue Whether this is an l-value that should be loaded.
/// @param ctx The code generation context.
/// @return The loaded value, or the original value.
llvm::Value* loadIfNeeded(
    llvm::Value* value,
    bool isLValue,
    CodeGenContext& ctx
);

// ─── Panic ─────────────────────────────────────────────────────────────────

/// @brief Emit a runtime panic call.
/// @param message The panic message.
/// @param ctx The code generation context.
/// @note This creates an unreachable instruction after the panic call.
void emitPanic(
    const std::string& message,
    CodeGenContext& ctx
);

// ─── Type Helpers ─────────────────────────────────────────────────────────

/// @brief Get the LLVM type for a declaration's type.
/// @param decl The declaration.
/// @param ctx The code generation context.
/// @return The LLVM type, or nullptr on error.
llvm::Type* getDeclType(
    const ValueDeclAST* decl,
    CodeGenContext& ctx
);

// ─── Name Helpers ─────────────────────────────────────────────────────────

/// @brief Get a mangled name for a declaration.
/// @param decl The declaration.
/// @param ctx The code generation context.
/// @return The mangled name string.
std::string getMangledName(
    const FuncDeclAST* decl,
    CodeGenContext& ctx
);

} // namespace codegen