/// @file IntrinsicEmitter.hpp
/// @brief Intrinsic emission dispatcher - routes to appropriate emitter.
///
/// This is the public API for intrinsic emission. It dispatches to either
/// LLVMIntrinsicEmitter or LucidIntrinsicEmitter based on the intrinsic's
/// IntrinsicEmitterKind, resolved once via IntrinsicRegistry.
///
/// isLLVMIntrinsic()/isLucidIntrinsic() used to live here as two
/// independently-maintained hardcoded unordered_set<std::string>. They're
/// gone - that classification is now IntrinsicEmitterKind, registry data
/// queried through IntrinsicRegistry::getEmitterKind(), so there's one
/// place this information lives instead of two.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/memory/InternedString.hpp"
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
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

/// @brief Emit an intrinsic call to LLVM IR by name.
/// @param name The intrinsic name, as the InternedString already carried
///        by IntrinsicCallExprAST::intrinsicName - never look this up as
///        a std::string just to dispatch; IntrinsicRegistry::getInfo(name)
///        resolves it to IntrinsicKind/IntrinsicEmitterKind directly.
/// @param args The argument values.
/// @param expr Optional AST node (for location and type info).
/// @param ctx The code generation context.
/// @return The LLVM value, or nullptr if the intrinsic is void or invalid.
llvm::Value* emitIntrinsic(
    InternedString name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
);

} // namespace codegen