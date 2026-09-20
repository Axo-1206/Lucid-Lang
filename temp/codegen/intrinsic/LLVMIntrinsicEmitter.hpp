/// @file LLVMIntrinsicEmitter.hpp
/// @brief Emission of LLVM intrinsics to LLVM IR.
/// 
/// This file handles intrinsics that map directly to LLVM intrinsic functions:
/// - Floating-Point Math: sqrt, fma, ceil, floor, round, etc.
/// - Memory Operations: memcpy, memmove, memset
/// - Bit Manipulation: clz, ctz, popcount, bswap
/// - SIMD Operations: simd_add, simd_mul, simd_load, etc.
/// - Atomics: atomic_load, atomic_store, atomic_add, etc.
/// - CPU Hints: prefetch, prefetch_r, prefetch_w, fence, pause
///
/// Every function here takes an IntrinsicKind alongside `name` - dispatch
/// happens on `kind` (a cheap enum comparison/switch, resolved once by the
/// caller via IntrinsicRegistry), not on repeated `name == "..."` string
/// comparisons. `name` is kept only for building diagnostic message text.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/registry/IntrinsicRegistry.hpp"
#include <llvm/IR/Value.h>
#include <string>
#include <vector>

namespace codegen {

// ─── LLVM Intrinsic Emitters ─────────────────────────────────────────────

/// @brief Emit a floating-point math LLVM intrinsic.
/// @param kind Which intrinsic (Sqrt, Abs, Fma, Ceil, Floor, Round, Pow, Min, Max).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMMathIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a memory operation LLVM intrinsic.
/// @param kind Which intrinsic (Memcpy, Memmove, Memset).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMMemoryIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a bit manipulation LLVM intrinsic.
/// @param kind Which intrinsic (Clz, Ctz, Popcount, Bswap).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMBitIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit an atomic operation LLVM intrinsic.
/// @param kind Which intrinsic (AtomicLoad, AtomicStore, AtomicAdd, etc.).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMAtomicIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a SIMD/vector LLVM intrinsic.
/// @param kind Which intrinsic (SimdAdd, SimdLoad, SimdSplat, etc.).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMSIMDIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a CPU hint LLVM intrinsic.
/// @param kind Which intrinsic (Prefetch, PrefetchR, PrefetchW, Fence, Pause).
/// @param name The intrinsic name, for diagnostic messages only.
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMCPUHintIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

} // namespace codegen