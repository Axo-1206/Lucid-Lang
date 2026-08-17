/// @file LucidIntrinsicEmitter.hpp
/// @brief Emission of Lucid-specific intrinsics to LLVM IR.
/// 
/// This file handles intrinsics that are implemented by the Lucid compiler
/// itself, not directly mapping to LLVM intrinsics:
/// - Type Inspection: sizeof, alignof, typeof, nameof, tostr, ptrstr
/// - Pointer Operations: toRef, toPtr, ptrOffset, ptrDiff, addrof
/// - Memory Management: alloc, free, arena_create, arena_alloc, arena_reset, arena_free
/// - String Operations: str_len, str_ptr, str_from_ptr, str_concat, str_slice, str_eq, str_byte_at
/// - Control Flow: likely, unlikely, scope_exit

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include <llvm/IR/Value.h>
#include <string>
#include <vector>

namespace codegen {

// ─── Lucid Intrinsic Emitters ────────────────────────────────────────────

/// @brief Emit a type inspection intrinsic.
/// @param name The intrinsic name (sizeof, alignof, typeof, nameof, tostr, ptrstr, bitcast).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidTypeIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a pointer operation intrinsic.
/// @param name The intrinsic name (toRef, toPtr, ptrOffset, ptrDiff, addrof).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidPointerIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a memory management intrinsic.
/// @param name The intrinsic name (alloc, free, arena_create, arena_alloc, arena_reset, arena_free).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidMemoryMgmtIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a string operation intrinsic.
/// @param name The intrinsic name (str_len, str_ptr, str_from_ptr, str_concat, str_slice, str_eq, str_byte_at).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidStringIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a control flow intrinsic.
/// @param name The intrinsic name (likely, unlikely, scope_exit).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidControlIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit the call for a single registered #scope_exit callback.
/// Invoked by lowerBlockStmt at block exit, in LIFO order, for each
/// registration collected by validateScopeExit during Sema.
/// @param reg The scope-exit registration (callback decl or closure expr, plus args).
/// @param ctx The code generation context.
void emitScopeExitCallback(
    const ScopeExitRegistration* reg,
    CodeGenContext& ctx
);

} // namespace codegen