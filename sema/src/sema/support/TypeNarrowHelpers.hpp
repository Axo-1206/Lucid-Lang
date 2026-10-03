/// @file TypeNarrowHelpers.hpp
/// @brief Narrowing detection for `T?` values in `if` conditions.
///
/// ─── What narrowing is ────────────────────────────────────────────────────
/// A value of type `T?` may hold `nil`. Using it where a `T` is expected
/// is a compile error unless the compiler can prove the value is not
/// `nil` at the use site. The compiler proves it by analyzing the shape
/// of an enclosing `if` condition:
///
///   - `if x != nil { ... }` — inside the then-branch, `x` is `T`.
///   - `if x == nil { ... } else { ... }` — inside the else-branch,
///     `x` is `T`.
///   - `if x == nil { return }` — after the if, `x` is `T`.
///   - `if x != nil and y != nil { ... }` — both are `T` in the then.
///   - `if x == nil or y == nil { ... }` — no narrowing in the then
///     (the disjunction does not prove either is non-nil), but the
///     *inverse* narrowing — both are `T` — applies in the else-branch.
///
/// The detector reads the condition's AST shape (identifier on one side,
/// `nil` literal on the other, `==` or `!=` as the operator) and returns
/// a `NarrowingInfo` describing what narrows and in which direction.
///
/// ─── What this file does NOT do ───────────────────────────────────────────
/// No fallible narrowing. The old grammar had `T!` (a fallible type)
/// and `err` (its counterpart to `nil`), and the narrowing detector
/// handled both. The new grammar has no `T!`, no `err`. Narrowing is
/// only over `nil`.
///
/// No `&T` narrowing. A row reference is inherently nilable, but the
/// grammar does not require it to be narrowed before dereferencing —
/// a dereference of a `nil` `&T` is a runtime panic, not a compile
/// error (§5.2). The narrowing detector does not produce narrowings for
/// `&T` values; a program that checks `if x != nil` on a `&T` gets a
/// runtime check, not a compile-time narrowing.
///
/// No `await`/`join` narrowing. The old grammar's `Future<T>` and
/// `Thread<T>` narrowed to `T` after the corresponding await/join. The
/// new grammar has neither.

#pragma once

#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "core/memory/InternedString.hpp"
#include "sema/context/SemaContext.hpp"

#include <unordered_map>

namespace lucid::sema {

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Detect the narrowing pattern of a binary expression.
///
/// This is the entry point called from `resolveBinaryExpr` when the
/// binary expression is an `if` condition. It recognizes the two
/// narrowing shapes (`x == nil`, `x != nil`), checks that the identifier
/// refers to a `T?` value (narrowing a non-nullable value is a no-op,
/// and the detector reports no narrowing), and returns a `NarrowingInfo`
/// describing the effect.
///
/// For a condition that is a conjunction (`and`) of narrowings, the
/// result combines them if they all use the same operator. A mixed
/// condition (`x != nil and y == nil`) is rejected with a diagnostic and
/// produces an empty `NarrowingInfo`.
///
/// For a condition that is a disjunction (`or`) of narrowings, the
/// result is empty for the then-branch (the disjunction proves nothing
/// about either operand in isolation), and the *inverse* — both are
/// narrowed — applies in the else-branch. The caller handles the
/// inverse application via `ScopedNarrowing`.
NarrowingInfo detectNarrowingPattern(BinaryExprAST* binary, SemaContext& ctx);

/// @brief Extract every narrowing from a condition expression.
///
/// Handles the full shape of a condition:
///   - a single `x == nil` / `x != nil` binary;
///   - an `and`-chain of such binaries;
///   - an `or`-chain of such binaries;
///   - `not x` where `x` is a `T?` value (treated as inverse
///     narrowing — the condition is true when `x` is nil).
///
/// @param expr     The condition expression.
/// @param ctx      The session context.
/// @param outMixed Optional. On return, `*outMixed` is `true` if the
///                 condition mixes `==` and `!=` in a way that makes
///                 narrowing unsound (a diagnostic is emitted by the
///                 caller in that case); `false` otherwise.
NarrowingInfo extractNarrowingsFromCondition(ExprAST* expr,
                                             SemaContext& ctx,
                                             bool* outMixed = nullptr);

} // namespace lucid::sema