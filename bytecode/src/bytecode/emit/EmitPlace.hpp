/**
 * @file compile/EmitPlace.hpp
 *
 * @responsibility Resolve an lvalue to a place and emit the store that
 *                 writes to it. The value/place split is the classic
 *                 stack-machine discipline: emitExpr produces a value,
 *                 emitPlace produces the operands a store needs.
 *
 * ─── Two functions ────────────────────────────────────────────────────────
 * The caller (EmitStmt's assignment case) does:
 *
 *     emitPlace(lhs, ctx);           // operands for the store
 *     emitExpr(rhs, ctx);            // the value
 *     emitStoreIntoPlace(lhs, ctx);  // the store opcode
 *
 * emitPlace emits whatever *operands* the store needs (a row
 * reference, an array, an index). For a local store, the slot is an
 * operand of the store opcode, not a stack value, so emitPlace emits
 * nothing — the opcode carries the slot directly.
 *
 * emitStoreIntoPlace emits the store opcode itself, given that the
 * operands and the value are already on the stack.
 *
 * ─── Why the split ────────────────────────────────────────────────────────
 * The split lets the caller compute the RHS *after* the place's
 * operands, so a compound assignment `x += f()` can evaluate the
 * place's operands once, evaluate `f()` once, add, and store. The
 * two-function form is what makes that possible.
 */

#pragma once

#include "../compile/CompilerContext.hpp"

#include "core/ast/ExprAST.hpp"

namespace lucid::bytecode::compile {

/// @brief Emit the operands a store needs, leaving them on the value
///        stack. Does NOT emit the store opcode. Does nothing for a
///        local or top-level binding, whose slot/offset is an operand
///        of the store opcode.
void emitPlace(ExprAST* lhs, CompilerContext& ctx);

/// @brief Emit the store opcode. Preconditions: the operands emitted
///        by emitPlace (if any) are on the stack, and the value to
///        store is on top of them.
void emitStoreIntoPlace(ExprAST* lhs, CompilerContext& ctx);

} // namespace lucid::bytecode::compile