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
 * Every per-form emitter returns bool: true if the statement transfers
 * control out of the enclosing block (a return, break, or continue),
 * false otherwise. emitStmt's return value is the same flag. Callers
 * use it to decide whether subsequent statements are reachable. Phase 3
 * ignores the flag (dead code is emitted and never executed); the flag
 * is the interface a future dead-code-elimination pass will use.
 *
 * ─── Design: scopes and drops ─────────────────────────────────────────────
 * emitBlock pushes a scope on the slot allocator when it enters, and
 * pops it on exit. The scope records the resource-typed slots the
 * block declared. On the block's fall-through exit, DropSchedule emits
 * a drop for each. Early exits (return, break, continue) emit their
 * own drops for the scopes they exit — the block's fall-through drops
 * are dead code on those paths.
 *
 * See memory/DropSchedule.hpp for the drop-emission logic.
 */

#pragma once

#include "../compile/CompilerContext.hpp"

#include "core/ast/StmtAST.hpp"

namespace lucid::bytecode::compile {

// ─── Dispatcher ─────────────────────────────────────────────────────────────

/// Lower a statement.
///
/// @return true if the statement transfers control out of the enclosing
///         block (a return, break, or continue); false otherwise.
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