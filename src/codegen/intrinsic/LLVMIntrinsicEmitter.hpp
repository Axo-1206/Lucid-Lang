/// @file codegen/intrinsic/LLVMIntrinsicEmitter.hpp
/// @brief Intrinsics that map to LLVM's own intrinsic functions.
///
/// ─── What Counts as an "LLVM-Side" Intrinsic ──────────────────────────────
/// An intrinsic is LLVM-side if its registry entry has an `llvmID`. These
/// are intrinsics that LLVM provides first-class support for:
///
///   - Math: `#sqrt`, `#sin`, `#cos`, `#tan`, `#fabs`, `#floor`, `#ceil`,
///     `#round`, `#exp`, `#log`, `#pow`.
///   - Bit manipulation: `#clz`, `#ctz`, `#popcount`, `#bswap`.
///   - Atomics: `#atomic_load`, `#atomic_store`, `#atomic_add`, etc.
///
/// The emitter's job is to call `llvm::Intrinsic::getDeclaration` for the
/// intrinsic ID and the argument types, then emit the call. There is no
/// custom IR beyond the LLVM intrinsic itself.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT a runtime wrapper. LLVM intrinsics are resolved at codegen
/// time; there is no `__lucid_*` symbol. The interpreter and AOT backends
/// see the intrinsic as a first-class LLVM construct.

#pragma once

#include "codegen/ownership/Ownership.hpp"

#include "core/registry/IntrinsicRegistry.hpp"
#include "core/ast/ExprAST.hpp"

namespace codegen {

class Emitter;

/// @brief Emit an LLVM-mapped intrinsic call.
///
/// The registry's `llvmID` is used to get the intrinsic's declaration
/// from LLVM. Each argument is emitted and passed to the intrinsic.
/// Result types are intrinsic-dependent; the registry's return type
/// determines the `Val`'s type.
Val emitLLVMIntrinsic(IntrinsicCallExprAST* expr,
                      const IntrinsicInfo& info,
                      Emitter& emitter);

} // namespace codegen