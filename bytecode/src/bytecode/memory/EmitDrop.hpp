/**
 * @file bytecode/memory/EmitDrop.hpp
 *
 * @responsibility Emit the instructions that release a value's
 *                 resources. The value is expected on top of the
 *                 value stack; the drop consumes it.
 *
 * ─── What a drop does ─────────────────────────────────────────────────────
 * A drop is the inverse of a copy:
 *
 *   - None: nothing to release; the value is discarded.
 *   - Discard: Ext_Pop discards the value (used for a discarded
 *     result whose type owns no resources).
 *   - FreeString / FreeArray: Ext_RtCall <RuntimeOp::FreeString |
 *     FreeArray> frees the heap buffer.
 *   - Release: Ext_RtCall <RuntimeOp::Release> decrements the host
 *     handle's refcount.
 *   - ElementWise: walk a fixed array's elements and drop each.
 *     The walk is unrolled (N is known at compile time). The array
 *     itself owns no heap buffer; it is discarded after its
 *     elements are dropped.
 *
 * ─── Stack effect ─────────────────────────────────────────────────────────
 * emitDrop consumes the value on top of the stack. After the call,
 * the value stack has one fewer entry, and the ownership stack has
 * one fewer entry (via the opcode's auto-bookkeeping).
 *
 * emitDropIfOwned reads the top ownership entry first. If it is
 * Owned, it emits the drop. Otherwise the value is discarded with
 * Ext_Pop. In both cases, exactly one ownership entry is consumed.
 */

#pragma once

#include "contract/ResourcePlan.hpp"
#include "OwnedValue.hpp"

#include "bytecode/compile/CompilerContext.hpp"

namespace lucid::bytecode::memory {

/// @brief Emit a drop of the value on top of the value stack.
///
/// The value's type is required: the drop plan is derived from it
/// via planForType, and the ElementWise case needs the type to know
/// the element count and the element's plan.
///
/// Preconditions (asserted):
///   - The value stack is non-empty.
///   - The ownership stack's top corresponds to that value.
///
/// Postconditions: the value is consumed; the value stack and
/// ownership stack each have one fewer entry.
void emitDrop(compile::CompilerContext& ctx,
              const contract::TypeDescriptor& type);

/// @brief Emit a drop only if the value actually owns resources
///        (its ownership flag is Owned). Used at scope exit, where a
///        slot may have been moved out earlier.
void emitDropIfOwned(compile::CompilerContext& ctx,
                     const contract::TypeDescriptor& type);

} // namespace lucid::bytecode::memory