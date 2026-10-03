/**
 * @file bytecode/memory/EmitCopy.hpp
 *
 * @responsibility Emit the instructions that produce a copy of a value
 *                 on the value stack. The copy is deep for resource-
 *                 owning types, a bit copy for primitives, and a
 *                 refcount retain for host handles.
 *
 * ─── What "copy" means here ───────────────────────────────────────────────
 * A copy is a *new value* on the stack that shares nothing with the
 * original. For a `string`, the copy owns a fresh heap buffer; the
 * original is unchanged. For a `Refcounted` host handle, the copy is
 * a retain — the handle shares the underlying object but the refcount
 * is incremented so both handles are valid.
 *
 * ─── Two callers ──────────────────────────────────────────────────────────
 *   1. Assignment: `y = x` where the RHS is an lvalue that must not
 *      be moved out (the source is still used later).
 *   2. Argument passing: `f(x)` where the callee's parameter is by
 *      value (the callee owns its copy, the caller keeps its own).
 *
 * ─── Stack effect ─────────────────────────────────────────────────────────
 * emitCopy expects the value to be on top of the stack. After the
 * call, the stack has two values: the original and the copy. The
 * caller decides which is which.
 *
 * For `BitCopy`, the copy is `Ext_Dup`.
 * For `Retain`, the copy is `Ext_Retain` — the value stays on the
 *   stack and its refcount is incremented, then `Ext_Dup` duplicates
 *   the handle value (two handles pointing at the same object, both
 *   with a refcount).
 * For heap-copy kinds, a runtime-call opcode is emitted.
 *
 * ─── Design: not all kinds are implemented yet ────────────────────────────
 * The `DeepCopyString` and `DeepCopyArray` cases need runtime-call
 * opcodes that aren't in the enum yet. They assert with a message
 * naming the missing opcode. Every other case is complete.
 */

#pragma once

#include "ResourcePlan.hpp"
#include "OwnedValue.hpp"

#include "bytecode/compile/CompilerContext.hpp"

namespace lucid::bytecode::memory {

/// @brief Emit a copy of the value on top of the value stack.
///
/// Preconditions (asserted):
///   - The value stack is non-empty (there is a value to copy).
///   - The ownership stack's top corresponds to that value.
///
/// After this call, the value stack has one more value (the copy),
/// and the ownership stack has one more entry (`Owned`).
void emitCopy(compile::CompilerContext& ctx, const ResourcePlan& plan);

} // namespace lucid::bytecode::memory