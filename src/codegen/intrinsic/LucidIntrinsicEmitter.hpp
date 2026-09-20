/// @file codegen/intrinsic/LucidIntrinsicEmitter.hpp
/// @brief Intrinsics that CodeGen implements directly.
///
/// ─── What Counts as a "Lucid-Side" Intrinsic ──────────────────────────────
/// An intrinsic is Lucid-side if CodeGen emits IR for it directly, without
/// going through LLVM's intrinsic machinery. This includes:
///
///   - Compile-time intrinsics: `#sizeof`, `#alignof`, `#typeof`, `#nameof`.
///     These are normally folded by Sema; if they reach the emitter, they
///     are evaluated at that point and emitted as constants.
///
///   - Runtime intrinsics that call the Lucid runtime: `#tostr`, `#ptrstr`.
///     These dispatch to `__lucid_int_to_str`, `__lucid_float_to_str`, etc.
///
///   - Memory intrinsics that call LLVM's `llvm.memcpy` etc.: `#memcpy`,
///     `#memmove`, `#memset`. These are technically LLVM intrinsics but
///     they're emitted through dedicated helpers rather than through the
///     generic LLVM-intrinsic path, because their argument marshalling is
///     special-cased.
///
///   - Pointer intrinsics: `#toRef`, `#toPtr`, `#ptrOffset`, `#ptrDiff`.
///     These do pointer arithmetic and boundary crossing.
///
///   - Scope intrinsics: `#scope_exit`. The call site emits nothing; the
///     callback is registered with the current block by Sema and emitted
///     at scope exit by the emitter's cleanup path.
///
///   - Allocation intrinsics: `#alloc`, `#free`, `#arena_*`. These call
///     the runtime's allocator directly.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT the emitter. It emits intrinsic calls; it doesn't walk the
/// AST or manage scopes.

#pragma once

#include "codegen/ownership/Ownership.hpp"

#include "core/registry/IntrinsicRegistry.hpp"
#include "core/ast/ExprAST.hpp"

namespace codegen {

class Emitter;

/// @brief Emit a Lucid-side intrinsic call.
///
/// `info` is the registry entry, already looked up by the dispatcher.
/// The emitter uses it for any per-intrinsic metadata it needs beyond
/// the name.
Val emitLucidIntrinsic(IntrinsicCallExprAST* expr,
                       const IntrinsicInfo& info,
                       Emitter& emitter);

} // namespace codegen