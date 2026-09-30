// ─── compile/EmitExpr.hpp ──────────────────────────────────────────────────
/**
 * @file compile/EmitExpr.hpp
 *
 * @responsibility Lower an expression into instructions, leaving its
 *                 result on the stack.
 */

#pragma once

#include "core/ast/ExprAST.hpp"
#include "CompilerContext.hpp"

namespace lucid::bytecode::compile {

void emitExpr(ExprAST* expr, CompilerContext& ctx);

} // namespace lucid::bytecode::compile