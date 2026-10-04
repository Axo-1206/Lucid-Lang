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
 * The ownership stack grows by one entry — the copy's. The original's
 * entry is untouched. When the copy owns a resource (a heap buffer, a
 * retained handle), the emitter marks it Owned; otherwise it stays
 * the default BitCopy.
 *
 *   - BitCopy / Reference: the copy is Ext_Dup. The auto-pushed
 *     BitCopy entry is correct; no mark.
 *   - Retain: Ext_RtCall <Retain> increments the refcount, then
 *     Ext_Dup duplicates the handle. The copy owns a refcount, so
 *     the emitter marks it Owned.
 *   - DeepCopyString / DeepCopyArray: Ext_Dup then
 *     Ext_RtCall <CopyString | CopyArray> produces a fresh heap
 *     value. The fresh value owns a resource, so the emitter marks
 *     it Owned.
 *
 * ─── Design: only ElementWise is unimplemented ────────────────────────────
 * Every CopyKind except `ElementWise` is implemented. The
 * `DeepCopyString`, `DeepCopyArray`, and `Retain` cases emit
 * `Ext_RtCall <RuntimeOp::CopyString | CopyArray | Retain>`, which
 * exist in RuntimeOp.hpp. The `ElementWise` case — a fixed-size
 * aggregate with resource-typed elements — asserts; its lowering
 * needs an element walk that is not yet written.
 */

#pragma once

#include "contract/ResourcePlan.hpp"
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
void emitCopy(compile::CompilerContext& ctx, const contract::ResourcePlan& plan);

} // namespace lucid::bytecode::memory