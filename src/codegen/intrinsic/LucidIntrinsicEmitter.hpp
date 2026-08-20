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
///
/// Every function here takes an IntrinsicKind alongside `name` - dispatch
/// happens on `kind` (a cheap enum comparison/switch, resolved once by the
/// caller via IntrinsicRegistry), not on repeated `name == "..."` string
/// comparisons. `name` is kept only for building diagnostic message text.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include <llvm/IR/Value.h>
#include <string>
#include <vector>

namespace codegen {

// ─── Lucid Intrinsic Emitters ────────────────────────────────────────────

/// @brief Emit a type inspection intrinsic.
/// @param kind Which intrinsic (Sizeof, Alignof, Typeof, Nameof, Tostr, Ptrstr, Bitcast).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidTypeIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a pointer operation intrinsic.
/// @param kind Which intrinsic (ToPtr, PtrOffset, PtrDiff - Addrof/ToRef
///        are special-cased earlier and never reach here).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidPointerIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a memory management intrinsic.
/// @param kind Which intrinsic (Alloc, Free, ArenaCreate, ArenaAlloc, ArenaReset, ArenaFree).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidMemoryMgmtIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a string operation intrinsic.
/// @param kind Which intrinsic (StrLen, StrPtr, StrFromPtr, StrConcat, StrSlice, StrEq, StrByteAt).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidStringIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a control flow intrinsic.
/// @param kind Which intrinsic (Likely, Unlikely, ScopeExit).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLucidControlIntrinsic(
    IntrinsicKind kind,
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