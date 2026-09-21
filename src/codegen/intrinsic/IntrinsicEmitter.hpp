/// @file codegen/intrinsic/IntrinsicEmitter.hpp
/// @brief The intrinsic dispatcher — the one entry point for `#intrinsics`.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The dispatcher for intrinsic calls. `emitIntrinsicFromAST` is called by
/// the emitter's `emit(IntrinsicCallExprAST*)` and routes the call to
/// either the Lucid-side emitter (for intrinsics CodeGen implements
/// directly) or the LLVM-side emitter (for intrinsics that map to LLVM's
/// own intrinsics).
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT the intrinsic registry. The registry (in
/// `core/registry/IntrinsicRegistry.hpp`) is the table of names, argument
/// types, and LLVM IDs. It's owned by Sema (which validates intrinsic
/// calls) and read here (for the LLVM ID of LLVM-mapped intrinsics).
///
/// It is NOT the emitter. The intrinsic dispatcher is a helper that the
/// emitter calls into; it takes an `Emitter&` and reads through it.
///
/// ─── Why `Emitter&`, Not `ProgramState&` ──────────────────────────────────
/// Intrinsics need the builder, the module, the ABI, the types, and the
/// diagnostics. All of these are reachable through `Emitter&`, which
/// holds a `ProgramState&`. Passing `Emitter&` instead of `ProgramState&`
/// is a signal: intrinsics are part of the emitter's vocabulary, not a
/// peer of it. It also keeps the interface narrow — the intrinsic
/// subsystem cannot reach into scopes, loops, or the value map, which it
/// has no business touching.

#pragma once

#include "codegen/ownership/Ownership.hpp"   // for `Val`

#include "core/ast/ExprAST.hpp"
#include "core/registry/IntrinsicRegistry.hpp"

namespace codegen {

class Emitter;

/// @brief Emit an intrinsic call.
///
/// Looks up the intrinsic's name in the registry, decides whether it's a
/// Lucid-side or LLVM-side intrinsic, and delegates to the appropriate
/// emitter. Returns the emitted value:
///
///   - Non-void intrinsic: a `Val` with the intrinsic's result type and
///     `Own::Owned` (a fresh value the caller takes over).
///   - Void intrinsic: an invalid `Val` (v == nullptr, ty == nullptr).
///
/// Emits a diagnostic and returns an invalid `Val` on error.
Val emitIntrinsicFromAST(IntrinsicCallExprAST* expr, Emitter& emitter);

// ─────────────────────────────────────────────────────────────────────────────
// Sub-emitter entry points
// ─────────────────────────────────────────────────────────────────────────────
//
// These are the two halves of the dispatcher. Each is implemented in its
// own translation unit (`LLVMIntrinsicEmitter.cpp` and
// `LucidIntrinsicEmitter.cpp`) and declared in its own header
// (`LLVMIntrinsicEmitter.hpp` and `LucidIntrinsicEmitter.hpp`). They are
// re-declared here so a caller that only includes `IntrinsicEmitter.hpp`
// can see the full dispatch surface without pulling in either sub-header.
//
// Both take the already-resolved `IntrinsicInfo` so they don't have to
// re-query the registry. Both return an invalid `Val` (not `nullptr`) on
// error, so the caller has one error convention to check.

/// @brief Emit an intrinsic that maps to an LLVM intrinsic.
///
/// The caller has already confirmed `info.llvmID.has_value()`. This
/// emitter lowers the intrinsic's arguments to `llvm::Value*`, resolves
/// the LLVM intrinsic declaration for `*info.llvmID`, and emits the call.
Val emitLLVMIntrinsic(IntrinsicCallExprAST* expr,
                      const IntrinsicInfo& info,
                      Emitter& emitter);

/// @brief Emit an intrinsic that CodeGen implements directly.
///
/// The caller has already confirmed `!info.llvmID.has_value()`. This
/// emitter dispatches on the intrinsic's `kind` (the
/// `IntrinsicEmitterKind::Lucid` rows in the registry) and lowers the
/// intrinsic to a sequence of LLVM IR operations, runtime ABI calls, or
/// both.
Val emitLucidIntrinsic(IntrinsicCallExprAST* expr,
                       const IntrinsicInfo& info,
                       Emitter& emitter);

} // namespace codegen