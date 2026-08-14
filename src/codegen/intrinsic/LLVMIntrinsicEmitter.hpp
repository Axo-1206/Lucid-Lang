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

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/ExprAST.hpp"
#include <llvm/IR/Value.h>
#include <string>
#include <vector>

namespace codegen {

// ─── LLVM Intrinsic Emitters ─────────────────────────────────────────────

/// @brief Emit a floating-point math LLVM intrinsic.
/// @param name The intrinsic name (sqrt, abs, fma, ceil, floor, round, pow, min, max).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMMathIntrinsic(
    const std::string& name,
    std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a memory operation LLVM intrinsic.
/// @param name The intrinsic name (memcpy, memmove, memset).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMMemoryIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a bit manipulation LLVM intrinsic.
/// @param name The intrinsic name (clz, ctz, popcount, bswap).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMBitIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit an atomic operation LLVM intrinsic.
/// @param name The intrinsic name (atomic_load, atomic_store, atomic_add, etc.).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMAtomicIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a SIMD/vector LLVM intrinsic.
/// @param name The intrinsic name (simd_add, simd_load, simd_splat, etc.).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMSIMDIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit a CPU hint LLVM intrinsic.
/// @param name The intrinsic name (prefetch, prefetch_r, prefetch_w, fence, pause).
/// @param args The argument values.
/// @param expr The AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if invalid.
llvm::Value* emitLLVMCPUHintIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

} // namespace codegen