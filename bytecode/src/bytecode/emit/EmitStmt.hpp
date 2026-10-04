/**
 * @file emit/EmitStmt.hpp
 *
 * @responsibility Lower a statement into instructions.
 *
 * ─── Scope ────────────────────────────────────────────────────────────────
 * emitStmt dispatches over every StmtAST subclass. The per-form
 * emitters (emitBlock, emitIfStmt, ...) are declared here and defined
 * in EmitStmt.cpp.
 *
 * ─── Design: the boolean return ───────────────────────────────────────────
 * Every per-form emitter returns bool: true if the statement
 * transfers control out of the enclosing block (a return, break, or
 * continue), false otherwise. The consumers:
 *
 *   - emitBlock: tracks whether any statement in the block
 *     transferred; returns true if so.
 *   - emitIfStmt: returns true only if both branches transfer (an
 *     if with no else always returns false — the false path falls
 *     through).
 *   - emitSwitchStmt: returns true only if every case body and the
 *     default body transfer.
 *   - emitWhileStmt, emitForStmt: return false. A loop may run zero
 *     iterations, so a loop never unconditionally transfers control
 *     out of its enclosing block.
 *   - emitReturnStmt, emitBreakStmt, emitContinueStmt: return true.
 *   - All other emitters: return false.
 *
 * The one consumer today is Compiler::compile, which uses the flag
 * from emitStmt(fn->body, ctx) to decide whether to emit an implicit
 * Ext_ReturnVoid on the fall-through path. If the body transfers
 * control out, that path is dead code and the implicit return would
 * double-drop the function's parameters.
 *
 * ─── Design: assignments are statements, not expressions ──────────────────
 * The grammar's `assign_stmt` is a statement form; there is no
 * `assign_expr`. An assignment is never nested inside another
 * expression. The AST reflects this: `AssignStmtAST` is a statement,
 * and there is no `AssignExprAST`.
 *
 * ─── Design: compound assignment ──────────────────────────────────────────
 * `x op= y` lowers to `x = x op y` with the place's operands evaluated
 * once. The lowering for each lvalue kind:
 *
 *   - Local slot:     LoadLocal <x>, <rhs>, <op>, then the store.
 *   - Field cell:     <row>, Ext_Dup, LoadField <col>, <rhs>, <op>,
 *                     then the store.
 *   - Index:          <arr>, Ext_Dup, <idx>, Ext_Dup, LoadIndex,
 *                     <rhs>, <op>, then the store.
 *
 * The Ext_Dups keep the place's operands alive for the store opcode,
 * which consumes them after LoadField/LoadIndex has consumed their
 * duplicates. The drop of the old value is handled by
 * emitStoreIntoPlace, exactly as for a plain assignment: the local
 * path drops the old slot's value; the field and index paths rely on
 * the interpreter's StoreField/StoreIndex.
 *
 * The ownership stack is kept in sync automatically; the compound
 * lowering does not push or pop ownership entries. See
 * EmitPlace.hpp's "Ownership bookkeeping" section for the store-side
 * mechanism.
 *
 * ─── Design: scopes and drops ─────────────────────────────────────────────
 * emitBlock pushes a scope on the slot allocator when it enters, and
 * pops it on exit. The scope records the resource-typed slots the
 * block declared. On the block's fall-through exit, DropSchedule emits
 * a drop for each.
 *
 * Early exits (return, break, continue) emit their own drops for the
 * scopes they exit. The block's fall-through drops are then
 * unreachable code — the bytecode emits them anyway, because Sema
 * owns dead-code elimination and the bytecode emitter lowers the full
 * body. The interpreter never executes the unreachable drops.
 *
 * See memory/DropSchedule.hpp for the drop-emission logic.
 *
 * ─── Design: ownership bookkeeping ────────────────────────────────────────
 * The ownership stack is kept in sync with the value stack by
 * CompilerContext::emitOpcode and noteStackEffect. Emitters do not
 * push or pop entries for the count. This file does not do so either,
 * with one exception:
 *
 *   - emitReturnStmt marks the return value's top entry as Moved
 *     (markTopAsMoved). This does not change the stack size; it
 *     overrides the entry's state so no drop below it touches the
 *     transferred value.
 *
 * Every other statement emitter relies on auto-bookkeeping. A store
 * (StoreLocal, StoreStaticData, StoreField, StoreIndex) auto-pops the
 * entries for the values it consumes. A control-flow opcode
 * (JumpIfFalse, JumpIfTrue, Jump, Pop) auto-pops the entries for any
 * values it discards.
 */

#pragma once

#include "../compile/CompilerContext.hpp"

#include "core/ast/StmtAST.hpp"

namespace lucid::bytecode::compile {

// ─── Dispatcher ─────────────────────────────────────────────────────────────

/// Lower a statement.
///
/// @return true if the statement transfers control out of the
///         enclosing block (a return, break, or continue); false
///         otherwise. See the file-level note above for the per-form
///         rules. The one consumer today is Compiler::compile.
bool emitStmt(StmtAST* stmt, CompilerContext& ctx);

// ─── Per-form emitters ──────────────────────────────────────────────────────

bool emitBlock       (BlockStmtAST*      stmt, CompilerContext& ctx);
bool emitVarDeclStmt (VarDeclStmtAST*    stmt, CompilerContext& ctx);
bool emitAssignStmt  (AssignStmtAST*     stmt, CompilerContext& ctx);
bool emitExprStmt    (ExprStmtAST*       stmt, CompilerContext& ctx);
bool emitReturnStmt  (ReturnStmtAST*     stmt, CompilerContext& ctx);
bool emitBreakStmt   (BreakStmtAST*      stmt, CompilerContext& ctx);
bool emitContinueStmt(ContinueStmtAST*   stmt, CompilerContext& ctx);
bool emitIfStmt      (IfStmtAST*         stmt, CompilerContext& ctx);
bool emitSwitchStmt  (SwitchStmtAST*     stmt, CompilerContext& ctx);
bool emitWhileStmt   (WhileStmtAST*      stmt, CompilerContext& ctx);
bool emitForStmt     (ForStmtAST*        stmt, CompilerContext& ctx);

// ─── Sequence suspend points ────────────────────────────────────────────────

bool emitWaitStmt          (WaitStmtAST*           stmt, CompilerContext& ctx);
bool emitWaitFramesStmt    (WaitFramesStmtAST*     stmt, CompilerContext& ctx);
bool emitWaitUntilStmt     (WaitUntilStmtAST*      stmt, CompilerContext& ctx);
bool emitWaitForEventStmt  (WaitForEventStmtAST*   stmt, CompilerContext& ctx);
bool emitWaitForRequestStmt(WaitForRequestStmtAST* stmt, CompilerContext& ctx);

} // namespace lucid::bytecode::compile