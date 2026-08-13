#pragma once

#include "../context/CodeGenContext.hpp"
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/BasicBlock.h>
#include <string>

namespace codegen {

/// @brief Get the length of an array at runtime.
/// @param target The array value (pointer for dynamic/fixed, or struct for slice).
/// @param arrayType The Lucid array type.
/// @param ctx The code generation context.
/// @return The length as an LLVM i64 value.
llvm::Value* getArrayLength(
    llvm::Value* target,
    const ArrayTypeAST* arrayType,
    CodeGenContext& ctx
);

}