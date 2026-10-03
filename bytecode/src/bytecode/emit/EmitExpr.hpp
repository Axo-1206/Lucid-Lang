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