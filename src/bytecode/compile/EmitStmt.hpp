// ─── compile/EmitStmt.hpp ──────────────────────────────────────────────────
/**
 * @file compile/EmitStmt.hpp
 *
 * @responsibility Lower a statement into instructions. Control-flow
 *                 skeleton only; expressions go through EmitExpr.
 */

#pragma once

#include "core/ast/StmtAST.hpp"
#include "CompilerContext.hpp"

namespace lucid::bytecode::compile {

void emitStmt(StmtAST* stmt, CompilerContext& ctx);

} // namespace lucid::bytecode::compile