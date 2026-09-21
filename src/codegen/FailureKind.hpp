/// @file codegen/FailureKind.hpp
/// @brief The closed set of runtime failures the compiler emits, and the
///        single predicate for "is this expression risky?".
///
/// ─── What This File Is ────────────────────────────────────────────────
/// Two things, both centralized:
///
///   1. `FailureKind` — an enum enumerating every kind of runtime
///      failure the compiler emits a check for. Each kind maps to a
///      `FailureInfo` row giving its diagnostic message, its runtime
///      counterpart, its `??`-interceptability, and the AST kinds whose
///      emitters can produce it.
///
///   2. `isRiskyLhs(expr, pool)` — the single source of truth for
///      "does this expression's evaluation potentially panic in a way
///      `??` can intercept?". Used by `Emitter::emitNullCoalesce`'s
///      dispatcher.
///
/// ─── Why Centralize ───────────────────────────────────────────────────
/// Before this file, the "is this expression risky?" predicate lived in
/// `EmitWrite.cpp`, and the `emitPanic` call sites were scattered across
/// five files. Keeping them in sync was manual and easy to get wrong:
/// adding a new runtime check required remembering to update the
/// predicate, and forgetting meant `??` silently no-opped.
///
/// With this file, adding a new runtime check is:
///   1. Add a `FailureKind` enumerator here.
///   2. Add its `FailureInfo` row in `FailureKind.cpp`.
///   3. Add its AST kind to the `isRiskyLhs` switch.
///   4. Call `emitter.emitFailure(FailureKind::X, loc)` at the check site.
///
/// Steps 1–3 all touch this file pair. If you do them together, they
/// can't drift.

#pragma once

#include "core/ast/BaseAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/memory/StringPool.hpp"
#include "runtime/RuntimeError.hpp"

#include <vector>

namespace codegen {

/// @brief One kind of runtime failure the compiler emits a check for.
///
/// Every `emitFailure` call uses one of these. Adding a new runtime check
/// means adding a new enumerator here and a row in `FailureKind.cpp`.
///
/// ─── Interceptable vs. Unconditional ──────────────────────────────────
/// The `FailureInfo::interceptableByCoalesce` flag distinguishes two
/// categories:
///
///   - Interceptable: an enclosing `??` on the expression containing
///     the check can branch to its fallback instead of panicking. These
///     are the checks whose failure is a *recoverable runtime condition*
///     (bounds, zero divisor, null deref, arena overflow).
///
///   - Unconditional: no `??` can catch the failure. These are checks
///     whose failure indicates a *bug* (internal invariant violation,
///     assertion failure), or whose failure happens outside the
///     compiler's control (runtime-internal panics).
///
/// Today, every enumerator is interceptable. The flag exists for
/// future unconditional checks; adding one is a new enumerator plus a
/// row with `interceptableByCoalesce = false`.
enum class FailureKind {
    // ─── Arithmetic ───────────────────────────────────────────────────
    DivisionByZero,               ///< Integer `/` or `/=` by zero.
    ModuloByZero,                 ///< Integer `%` or `%=` by zero.

    // ─── Array / slice indexing ───────────────────────────────────────
    FixedArrayIndexOutOfBounds,   ///< `[N]T[i]`, `i` outside `[0, N)`.
    SliceIndexOutOfBounds,        ///< `[_]T[i]`, `i` outside `[0, len)`.
    DynamicArrayIndexOutOfBounds, ///< `[*]T[i]`, `i` outside `[0, len)`.
    SliceBoundsOutOfRange,        ///< `arr[lo..hi]` with invalid bounds.

    // ─── Pointers ─────────────────────────────────────────────────────
    NullPointerDereference,       ///< `#toRef(p)` where `p` is null.

    // ─── SIMD ─────────────────────────────────────────────────────────
    SimdLaneOutOfBounds,          ///< `#simd_extract` / `#simd_insert`
                                  ///< lane index outside `[0, N)`.

    // ─── Arena ────────────────────────────────────────────────────────
    ArenaOutOfCapacity,           ///< `arena::alloc<T>(n)` overflow.
};

/// @brief Metadata for one `FailureKind`.
struct FailureInfo {
    /// The runtime-side kind, used to build the panic message.
    RuntimeErrorKind runtimeKind;

    /// True if an enclosing `??` on the expression that contains this
    /// check can intercept the failure. If false, `emitFailure` always
    /// panics, regardless of the tracker's state.
    bool interceptableByCoalesce;

    /// The AST kind(s) whose emitters can produce this failure. Used by
    /// `isRiskyLhs` as a first-level filter. Empty means "no expression
    /// can trigger this failure" — used for unconditional kinds that
    /// only fire from compiler-internal paths.
    std::vector<ASTKind> astKinds;

    /// Human-readable description, for documentation and testing.
    /// Not used to build the panic message (the runtime formats that).
    const char* description;
};

/// @brief Look up the metadata for a `FailureKind`.
///
/// Defined in `FailureKind.cpp`; the table lives there.
const FailureInfo& failureInfo(FailureKind kind);

/// @brief True if `expr` is a risky operation — an expression whose
///        value has a concrete type but whose evaluation can panic in a
///        way `??` can intercept.
///
/// This is the single source of truth for the question. It MUST agree
/// with the `FailureInfo` table: every `FailureKind` with
/// `interceptableByCoalesce == true` must have its AST kinds covered by
/// this function.
///
/// Consulted by `Emitter::emitNullCoalesce`'s dispatcher.
///
/// `pool` is needed for the intrinsic-name and arena-method-name
/// lookups.
bool isRiskyLhs(const ExprAST* expr, StringPool& pool);

} // namespace codegen