/// @file IntrinsicEmitter.hpp
/// @brief Intrinsic emission to LLVM IR.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/ExprAST.hpp"
#include <llvm/IR/Value.h>
#include <string>
#include <vector>

namespace codegen {

// ─── Main Entry Point ──────────────────────────────────────────────────────

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
/// @param expr Optional AST node (for scope_exit).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if the intrinsic is void or invalid.
llvm::Value* emitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

// ─── Helper ──────────────────────────────────────────────────────────────

/// @brief Get an LLVM intrinsic function declaration.
llvm::Function* getLLVMIntrinsicDecl(
    llvm::Intrinsic::ID id,
    llvm::ArrayRef<llvm::Type*> argTypes,
    CodeGenContext& ctx
);

} // namespace codegen