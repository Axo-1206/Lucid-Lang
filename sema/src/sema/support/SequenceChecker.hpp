/// @file sema/support/SequenceChecker.hpp
/// @brief The signature-and-body rules for `@sequence` functions.
///
/// ─── What this file is ────────────────────────────────────────────────────
/// A `@sequence` function is an ordinary `FN` with a `@sequence` attribute.
/// The attribute changes the rules the function must obey:
///
///   - it always returns `unit` (§9.2.5);
///   - it cannot have a `host(...)` body (§9.2.5);
///   - it must be launched with `start`, not called directly (§9.2.4);
///   - it cannot call another `@sequence` function directly (§9.2.5);
///   - it is not a valid function-typed value (§9.2.5);
///   - its body may contain the five `wait*` suspend points (§9.2.2).
///
/// Of those, two are *signature* rules — checkable from the function's
/// return type and body shape — and one is a *body* warning — checkable
/// after the body has been resolved. The other three are *use-site*
/// rules: they are about what a caller may do with a `@sequence`
/// function, not about the function itself. Those live at their call
/// sites in `SemaExpr.cpp` and `SemaStmt.cpp`, where the calling
/// context is available.
///
/// This file owns the signature rules and the body warning.
///
/// ─── Design: two entry points, one per pass ──────────────────────────────
/// The signature check runs during pass 2, right after the function's
/// signature has been resolved: the return type must be `unit`, and the
/// body (if any) must not be a `host(...)` target.
///
/// The body check runs during pass 3, right after the body has been
/// resolved: the body should actually suspend. A `@sequence` that never
/// suspends is almost certainly a mistake — the author probably meant
/// to write an ordinary `FN` — so the check is a warning, not an error.
///
/// The two entry points are separated because they need different
/// information at different times. The signature check needs the
/// resolved return type and the `isHostBound` flag, both available in
/// pass 2. The body check needs the fully-resolved body, which is only
/// available after pass 3 has walked it.
///
/// ─── Design: no "only suspends" warning ──────────────────────────────────
/// §9.2.5 could suggest a warning for the mirror-image case: a body
/// that does *nothing but* suspend. That shape is legitimate — a
/// sequence that just paces itself with `wait()` calls — and warning
/// on every one of them would be noise. The check is left out.
///
/// ─── Design: no use-site checks ──────────────────────────────────────────
/// The three use-site rules (direct call, `start` requires sequence,
/// sequence as a function value) are not here. They are checks at the
/// point of use, not at the point of declaration, and the calling
/// context is what makes them decidable. `resolveCallExpr` in
/// `SemaExpr.cpp` handles the first and third; `resolveStartExpr` in
/// the same file handles the second.

#pragma once

#include "core/ast/DeclAST.hpp"

#include "sema/context/SemaContext.hpp"

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Public entry points
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Check a `@sequence` function's signature (pass 2).
///
/// Called from `resolveFnDecl` after the signature has been resolved
/// and the `@sequence` attribute has been validated. The function's
/// `isSequence` flag must be set by then.
///
/// Emits:
///   - `Seq_SequenceReturnsValue` if the function has a non-`unit`
///     return type;
///   - `Seq_SequenceHasHostBody` if the function is host-bound.
///
/// A function that is not a `@sequence` is a no-op: every rule here
/// applies only to sequences.
///
/// @param fn   The function declaration to check.
/// @param ctx  The session context.
///
/// @return true if the signature is valid (or the function is not a
///         sequence). false if any diagnostic was emitted.
bool checkSequenceSignature(FnDeclAST* fn, SemaContext& ctx);

/// @brief Check a `@sequence` function's body (pass 3).
///
/// Called from `resolveFnBody` after the body has been resolved.
/// Emits a warning if the body contains no `wait*` suspend points: a
/// sequence that never suspends is almost certainly a mistake — the
/// author likely meant to write an ordinary `FN`.
///
/// A function that is not a `@sequence`, or is host-bound (so has no
/// body), is a no-op.
///
/// @param fn   The function declaration whose body was resolved.
/// @param ctx  The session context.
///
/// @return true if the body is fine (or the function is not a
///         sequence, or is host-bound). false if a warning was emitted.
bool checkSequenceBody(FnDeclAST* fn, SemaContext& ctx);

} // namespace lucid::sema