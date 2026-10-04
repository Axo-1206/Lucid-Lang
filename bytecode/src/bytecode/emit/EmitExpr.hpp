/**
 * @file compile/EmitExpr.hpp
 *
 * @responsibility Lower an expression into instructions, leaving its
 *                 result on the value stack.
 *
 * ─── Scope ────────────────────────────────────────────────────────────────
 * emitExpr dispatches over every ExprAST subclass. The per-form
 * emitters (emitLiteralExpr, emitBinaryExpr, ...) are declared here and
 * defined in EmitExpr.cpp.
 *
 * ─── Design: per-form functions are part of the compiler's API ────────────
 * The per-form functions have external linkage so the .cpp needs no
 * forward declarations and no anonymous namespace. They are part of
 * this header's API. Nothing outside bytecode/compile/ should call
 * them directly — the entry point is emitExpr — but they are visible
 * because there is no reason to hide them and every reason to keep the
 * .cpp's structure flat.
 *
 * ─── Design: assignments are not expressions ──────────────────────────────
 * Assignment is a statement in the grammar (§12). There is no
 * AssignExprAST and no per-form emitter for assignment here. The
 * assignment case is in EmitStmt.cpp.
 *
 * ─── Design: types are read, not re-derived ───────────────────────────────
 * Sema resolved every expression's type. The emitter reads
 * expr->resolvedType and translates it. It does not infer types and
 * does not re-check type compatibility. A node with a null
 * resolvedType is a Sema bug and the emitter asserts.
 *
 * ─── Design: function values are constants ────────────────────────────────
 * A bare function name (`isMinor`, `onIdleEnter`) is a compile-time
 * code address. It is not a load from a variable. emitIdentifierExpr
 * recognizes the resolvedDecl == FnDeclAST case and emits LoadFunction
 * with the function's index in the artifact. A lambda is the same: the
 * compiler already lowered it to a top-level function during a
 * pre-pass; emitLambdaExpr emits LoadFunction for that lowered
 * function.
 *
 * ─── Design: column views are compile-time identities ─────────────────────
 * A column view (`T.col`) has no runtime representation. Its two uses
 * — as a `for` iterable and as a `TOARRAY()` receiver — both consume
 * the table's artifact index and the column's index directly. The
 * `for` loop emits a LoadRow + LoadField per iteration; `TOARRAY`
 * emits a single Ext_ColumnToArray with both operands. No column-view
 * value ever appears on the stack.
 *
 * ─── Design: ownership of produced values ─────────────────────────────────
 * Every expression emitter leaves one value on the value stack.
 * CompilerContext::emitOpcode auto-pushes one BitCopy ownership entry
 * for that value. When the produced value owns a resource (a string,
 * a dynamic array, a host handle, a fixed array of resources), the
 * emitter calls ctx.owned().markTopAsOwned() to upgrade the entry.
 *
 * The two outcomes:
 *   - Produces a primitive, a row reference, or a function value:
 *     no mark. The auto-pushed BitCopy is correct.
 *   - Produces a resource-owning value: mark Owned.
 *
 * The rule applies at every leaf of the expression tree. A composite
 * expression's ownership is determined by its own type, not by its
 * sub-expressions' ownership — the sub-expression entries are consumed
 * by the outer opcode and replaced with a fresh entry for the
 * composite result.
 */

#pragma once

#include "../compile/CompilerContext.hpp"

#include "core/ast/ExprAST.hpp"

namespace lucid::bytecode::compile {

// ─── Dispatcher ─────────────────────────────────────────────────────────────

/// Lower an expression, leaving its result on the value stack.
///
/// Preconditions (asserted): expr is non-null and has a resolved type
/// (Sema ran to completion on its module).
void emitExpr(ExprAST* expr, CompilerContext& ctx);

// ─── Column-view receiver resolution ────────────────────────────────────────

/// @brief The TableDeclAST a column view is over.
///
/// A column view (`T.col` or `mod.T.col`) has a receiver that
/// resolves to a table declaration. The receiver is either an
/// IdentifierExprAST (bare) or a FieldAccessExprAST with
/// isModuleAccess set (qualified). This helper extracts the table
/// declaration from either shape.
///
/// Precondition (asserted): `colView` is a column view
/// (isColumnView is true).
///
/// A null return would mean Sema produced a column view whose
/// receiver does not resolve to a table; the assert fires before
/// the return.
const TableDeclAST* columnViewTable(const FieldAccessExprAST* colView);

// ─── Per-form emitters ──────────────────────────────────────────────────────
//
// One per ExprAST subclass. Each is called from emitExpr's dispatch and
// may be called directly by a caller that has already classified the
// node. Each leaves the expression's value on the stack.

void emitLiteralExpr      (LiteralExprAST*      e, CompilerContext& ctx);
void emitIdentifierExpr   (IdentifierExprAST*   e, CompilerContext& ctx);
void emitArrayLiteralExpr (ArrayLiteralExprAST* e, CompilerContext& ctx);
void emitFieldAccessExpr  (FieldAccessExprAST*  e, CompilerContext& ctx);
void emitIndexExpr        (IndexExprAST*        e, CompilerContext& ctx);
void emitCallExpr         (CallExprAST*         e, CompilerContext& ctx);
void emitLambdaExpr       (LambdaExprAST*       e, CompilerContext& ctx);
void emitStartExpr        (StartExprAST*        e, CompilerContext& ctx);
void emitUnaryExpr        (UnaryExprAST*        e, CompilerContext& ctx);
void emitBinaryExpr       (BinaryExprAST*       e, CompilerContext& ctx);
void emitParenExpr        (ParenExprAST*        e, CompilerContext& ctx);
void emitRangeExpr        (RangeExprAST*        e, CompilerContext& ctx);

} // namespace lucid::bytecode::compile