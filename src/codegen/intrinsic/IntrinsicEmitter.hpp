/// @file IntrinsicEmitter.hpp
/// @brief Intrinsic emission dispatcher - routes to appropriate emitter.
///
/// This is the public API for intrinsic emission. It dispatches to either
/// LLVMIntrinsicEmitter or LucidIntrinsicEmitter based on the intrinsic type.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/ExprAST.hpp"
#include <llvm/IR/Value.h>
#include <string>
#include <vector>

namespace codegen {

// ─── Main Entry Points ──────────────────────────────────────────────────────

/// @brief Emit an intrinsic call from an AST node.
/// @param expr The intrinsic call expression.
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if void or invalid.
llvm::Value* emitIntrinsicFromAST(
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit an intrinsic call to LLVM IR by name.
/// @param name The intrinsic name (e.g., "sqrt", "memcpy").
/// @param args The argument values.
/// @param expr Optional AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if the intrinsic is void or invalid.
llvm::Value* emitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

// ─── Helper ──────────────────────────────────────────────────────────────

/// @brief Check if an intrinsic is an LLVM intrinsic.
/// @param name The intrinsic name.
/// @return True if the intrinsic maps to an LLVM intrinsic.
bool isLLVMIntrinsic(const std::string& name);

/// @brief Check if an intrinsic is a Lucid compiler-handled intrinsic.
/// @param name The intrinsic name.
/// @return True if the intrinsic is handled by the Lucid compiler.
bool isLucidIntrinsic(const std::string& name);

} // namespace codegen