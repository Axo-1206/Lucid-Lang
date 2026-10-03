/**
 * @file compile/EmitStmt.hpp
 *
 * @responsibility Lower a statement into instructions.
 *
 * ─── Scope ────────────────────────────────────────────────────────────────
 * emitStmt dispatches over every StmtAST subclass. The per-form
 * resolvers (resolveBlock, resolveIfStmt, ...) are declared here and
 * defined in EmitStmt.cpp.
 *
 * ─── Design: per-form functions are part of the compiler's API ────────────
 * The per-form resolvers have external linkage so the .cpp needs no
 * forward declarations and no anonymous namespace. They are part of
 * this header's API. Nothing outside bytecode/compile/ should call
 * them directly — the entry point is emitStmt — but they are visible
 * because there is no reason to hide them and every reason to keep the
 * .cpp's structure flat.
 *
 * ─── Design: the boolean return ───────────────────────────────────────────
 * Every per-form resolver returns bool: true if the statement transfers
 * control out of the enclosing block (a return, break, or continue),
 * false otherwise. emitStmt's return value is the same flag for its
 * own statement. Callers use the flag to decide whether subsequent
 * statements are reachable; Phase 3 ignores it (dead code is emitted
 * and never executed), but the flag is the interface a future
 * dead-code-elimination pass will use.
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
void emitStmt(StmtAST* stmt, CompilerContext& ctx);

// ─── Per-form resolvers ─────────────────────────────────────────────────────
//
// One per StmtAST subclass. Each is called from emitStmt's dispatch and
// may be called directly by a caller that has already classified the
// node. All return the same bool protocol as emitStmt.

bool resolveBlock       (BlockStmtAST*      stmt, CompilerContext& ctx);
bool resolveVarDeclStmt (VarDeclStmtAST*    stmt, CompilerContext& ctx);
bool resolveAssignStmt  (AssignStmtAST*     stmt, CompilerContext& ctx);
bool resolveExprStmt    (ExprStmtAST*       stmt, CompilerContext& ctx);
bool resolveReturnStmt  (ReturnStmtAST*     stmt, CompilerContext& ctx);
bool resolveBreakStmt   (BreakStmtAST*      stmt, CompilerContext& ctx);
bool resolveContinueStmt(ContinueStmtAST*   stmt, CompilerContext& ctx);
bool resolveIfStmt      (IfStmtAST*         stmt, CompilerContext& ctx);
bool resolveSwitchStmt  (SwitchStmtAST*     stmt, CompilerContext& ctx);
bool resolveWhileStmt   (WhileStmtAST*      stmt, CompilerContext& ctx);
bool resolveForStmt     (ForStmtAST*        stmt, CompilerContext& ctx);

// ─── Sequence suspend points ────────────────────────────────────────────────

bool resolveWaitStmt          (WaitStmtAST*           stmt, CompilerContext& ctx);
bool resolveWaitFramesStmt    (WaitFramesStmtAST*     stmt, CompilerContext& ctx);
bool resolveWaitUntilStmt     (WaitUntilStmtAST*      stmt, CompilerContext& ctx);
bool resolveWaitForEventStmt  (WaitForEventStmtAST*   stmt, CompilerContext& ctx);
bool resolveWaitForRequestStmt(WaitForRequestStmtAST* stmt, CompilerContext& ctx);

} // namespace lucid::bytecode::compile