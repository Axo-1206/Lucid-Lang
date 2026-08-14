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
    ArrayTypeAST* arrayType,
    CodeGenContext& ctx
);

/// @brief Lower a range-based for loop.
/// @param stmt The for statement AST.
/// @param headerBlock The header block for condition checking.
/// @param bodyBlock The body block.
/// @param continueBlock The continue block for increment.
/// @param exitBlock The exit block.
/// @param ctx The code generation context.
void lowerRangeForLoop(
    ForStmtAST* stmt,
    llvm::BasicBlock* headerBlock,
    llvm::BasicBlock* bodyBlock,
    llvm::BasicBlock* continueBlock,
    llvm::BasicBlock* exitBlock,
    CodeGenContext& ctx
);

/// @brief Lower a collection-based for loop.
/// @param stmt The for statement AST.
/// @param headerBlock The header block for condition checking.
/// @param bodyBlock The body block.
/// @param continueBlock The continue block for increment.
/// @param exitBlock The exit block.
/// @param ctx The code generation context.
void lowerCollectionForLoop(
    ForStmtAST* stmt,
    llvm::BasicBlock* headerBlock,
    llvm::BasicBlock* bodyBlock,
    llvm::BasicBlock* continueBlock,
    llvm::BasicBlock* exitBlock,
    CodeGenContext& ctx
);

}