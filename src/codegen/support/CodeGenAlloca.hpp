/// @file support/CodeGenAlloca.hpp
/// @brief Memory allocation and basic block management helpers for code generation.

#pragma once

#include "../context/CodeGenContext.hpp"
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/BasicBlock.h>
#include <string>

namespace codegen {

/// @brief Create an alloca instruction in the current function's entry block.
/// @param name The name for the alloca (for debugging).
/// @param type The LLVM type to allocate.
/// @param ctx The code generation context.
/// @return The alloca instruction, or nullptr if no current function.
llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
);

/// @brief Create a new basic block in the current function.
/// @param name The name for the block (for debugging).
/// @param ctx The code generation context.
/// @return The new basic block, or nullptr if no current function.
llvm::BasicBlock* createBlock(
    const std::string& name,
    CodeGenContext& ctx
);

/// @brief Load a value from a pointer with explicit element type.
/// @param value The pointer value to load from.
/// @param elemType The element type (for opaque pointers).
/// @param ctx The code generation context.
/// @return The loaded value, or the original value if not a pointer.
llvm::Value* loadIfNeeded(
    llvm::Value* value,
    llvm::Type* elemType,
    CodeGenContext& ctx
);

/// @brief DEPRECATED: Use the overload with explicit elemType.
/// @param value The value to potentially load.
/// @param isLValue If true, load the value.
/// @param ctx The code generation context.
/// @return The loaded value if isLValue is true, otherwise the original value.
/// @deprecated With opaque pointers (LLVM 17+), the element type cannot be inferred.
llvm::Value* loadIfNeeded(
    llvm::Value* value,
    bool isLValue,
    CodeGenContext& ctx
);

} // namespace codegen