/**
 * @file interp/Ops.hpp
 *
 * @responsibility The declarations of every opcode-family handler.
 *
 * ─── Design: one function per family ──────────────────────────────────────
 * The dispatcher reads the opcode, maps it to a family, and calls
 * the family's handler. The handler switches on the specific opcode
 * and executes it. This keeps the dispatcher's switch small and each
 * family's logic in its own file.
 *
 * ─── Design: the handler owns the operand read and the ip advance ─────────
 * The dispatcher has already read the opcode and advanced past it
 * (opcodeStreamByte + escape). Each family handler reads its own
 * operands (via the OpHelpers) and advances the frame's ip past them.
 */

#pragma once

#include "contract/Opcode.hpp"

namespace lucid::interp {

class InterpreterInternal;

void opsLoadStore  (InterpreterInternal& interp, contract::Opcode op);
void opsArithmetic (InterpreterInternal& interp, contract::Opcode op);
void opsComparison (InterpreterInternal& interp, contract::Opcode op);
void opsControl    (InterpreterInternal& interp, contract::Opcode op);
void opsCall       (InterpreterInternal& interp, contract::Opcode op);
void opsAggregate  (InterpreterInternal& interp, contract::Opcode op);
void opsHost       (InterpreterInternal& interp, contract::Opcode op);
void opsRuntime    (InterpreterInternal& interp, contract::Opcode op);
void opsConcurrency(InterpreterInternal& interp, contract::Opcode op);

} // namespace lucid::interp