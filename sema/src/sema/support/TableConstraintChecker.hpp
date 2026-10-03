/// @file sema/support/TableConstraintChecker.hpp
/// @brief Every table-shape constraint check, in one place.
///
/// ─── What this file is ────────────────────────────────────────────────────
/// A table's attribute list and its shape can each be legal in isolation
/// while their combination is not. `@reserve(10)` is a well-formed
/// attribute; `TABLE X { } @reserve(10) @fixed` is not, because a fixed
/// table cannot grow and there is nothing to reserve room for. That
/// combination is not visible to `AttributeValidator`, which sees only
/// the attribute span, nor to `resolveColumnDecl`, which sees only one
/// column. It is visible once the whole table has been assembled:
/// attributes decoded, columns resolved, inline rows in hand.
///
/// This file runs at that point. It walks the table's shape and reports
/// every constraint violation it finds. It is the only place in Sema
/// where a table is checked as a whole.
///
/// ─── What this file checks ────────────────────────────────────────────────
/// Fifteen rules, drawn from §4.1.1b, §4.1.3, §4.1.4, §4.1.5, §4.1.1c,
/// and §7.1:
///
///   1.  `@packed` requires `@fixed` or `@readonly`
///   2.  `@fixed` and `@readonly` are mutually exclusive
///   3.  `@reserve(N)` is legal only on a growing table
///   4.  `@request` is legal only on a host-backed table
///   5.  `@request` also requires the host table to be a `host(...)` target
///   6.  A `@fixed`/`@readonly` table must have at least one row
///   7.  Column names are unique within the table
///   8.  At most one `@primary` column per table
///   9.  A `@primary` column's type is a valid key type
///   10. A `@primary` column is not nilable, not `&T`, not array, not function
///   11. A column or fixed-row member name does not equal a built-in method
///   12. The generated `by<Column>` name does not collide with a column or row
///   13. No two inline initializer rows share a `@unique`/`@primary` value
///   14. Every fixed-table cell is a compile-time constant
///   15. Fixed tables do not cyclically reference each other
///
/// ─── What this file does NOT check ────────────────────────────────────────
/// Attribute argument shapes (`@reserve` takes one integer, etc.) are
/// `AttributeValidator`'s job. Column type resolution is
/// `resolveColumnDecl`'s. The use-site rule "`ADD` is unavailable on a
/// `@readonly` table" is checked at each call site in `SemaExpr.cpp`.
///
/// ─── Design: one entry point, many static helpers ─────────────────────────
/// `checkTableConstraints` is the only public function. Everything else
/// — the fifteen helpers, the cycle detector, the key-type predicate —
/// is `static` in the `.cpp`. A future caller that wants a single rule
/// in isolation (the LSP resolving one constraint, a test) can add a
/// new entry point here; today there is one caller and no such need.
///
/// ─── Design: no caching ───────────────────────────────────────────────────
/// The cyclic-reference check (rule 15) performs a depth-first search
/// over the tables a fixed table references. The reachable set is small
/// for real code, and the check runs once per table during pass 2.
/// Caching the DFS result would require a `SemaContext` member that
/// outlives the check; the cost is not worth the complexity. If the
/// check becomes a hotspot, adding a cache is a local change to the
/// cycle detector.

#pragma once

#include "core/ast/DeclAST.hpp"
#include "core/ast/TypeAST.hpp"

namespace lucid::sema {

struct SemaContext;   // forward declaration; defined in SemaContext.hpp

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Run every table-shape constraint check on `table`.
///
/// Called from `resolveTableDecl` (pass 2), after the attributes have
/// been validated and every column's type has been resolved. At the
/// point this runs, the table's decoded fields are populated:
/// `isFixed`, `isReadonly`, `isPacked`, `isReserved`, `isHostBacked`,
/// `isRequest`, `columns`, `rows`. The checker reads those fields and
/// the columns' resolved types.
///
/// Emits diagnostics for every violation. A table with two violations
/// reports two diagnostics, not one; the checker does not stop at the
/// first failure.
///
/// @param table  The table declaration to check. Null is accepted and
///               is a no-op.
/// @param ctx    The session context.
///
/// @return true if no violation was found. false if any was; on false,
///         at least one diagnostic has been emitted. The caller uses
///         the return value only to decide whether to skip further work
///         on this table (setting a mangled name, for instance). The
///         diagnostics themselves have already been emitted regardless
///         of the return value.
bool checkTableConstraints(TableDeclAST* table, SemaContext& ctx);

} // namespace lucid::sema