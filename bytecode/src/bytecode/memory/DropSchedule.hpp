/**
 * @file bytecode/memory/DropSchedule.hpp
 *
 * @responsibility Orchestrate scope-exit drops. Given a scope (a
 *                 block, a function body, a loop body), emit drops
 *                 for every resource-typed slot it owns, in reverse
 *                 declaration order.
 *
 * ─── Why scope-exit drops ─────────────────────────────────────────────────
 * A local `let s: string = "hi"` allocates a heap buffer when the
 * initializer runs. The slot owns that buffer. When the scope ends,
 * the buffer must be freed. The compiler emits the drop at the scope
 * exit point.
 *
 * ─── Reverse order ────────────────────────────────────────────────────────
 * Drops are emitted in reverse declaration order. If two locals have
 * a dependency (rare with value semantics, but possible with host
 * types), the later one's drop runs first.
 *
 * ─── Exits ────────────────────────────────────────────────────────────────
 * A block can be exited in several ways:
 *   - The normal end of the block (fall through).
 *   - A `return` that leaves the function.
 *   - A `break` that leaves the enclosing loop.
 *   - A `continue` that starts the next iteration.
 *
 * Each exit emits drops for every scope it exits. A `return` from a
 * nested block emits drops for the nested block and every enclosing
 * block up to the function. A `break` emits drops for every block
 * between the break and the loop body.
 *
 * The drop schedule walks the slot allocator's open scopes (which
 * track the nesting) to produce the right set.
 *
 * ─── Ownership bookkeeping ────────────────────────────────────────────────
 * Each drop is self-balanced. dropSlot emits LoadLocal (auto-push one
 * BitCopy ownership entry), upgrades the entry to Owned via
 * markTopAsOwned, then calls emitDropIfOwned. The drop opcode's
 * Ext_RtCall auto-pops the entry via noteStackEffect. Net effect on
 * the ownership stack per drop: zero.
 *
 * The three entry points (emitScopeDrops, emitReturnDrops,
 * emitLoopExitDrops) call dropSlot in a loop; they add no ownership
 * entries of their own and pop none. The ownership stack size is the
 * same before and after each.
 */

#pragma once

#include "contract/ResourcePlan.hpp"

#include "bytecode/compile/CompilerContext.hpp"

namespace lucid::bytecode::memory {

class DropSchedule {
public:
    /// Emit drops for every resource-typed slot in the given scope,
    /// in reverse declaration order. Called at the normal end of a
    /// block.
    ///
    /// Preconditions: `scope` was returned by SlotAllocator::
    /// popScope, and the caller has already loaded each slot's value
    /// onto the stack (this function emits the drops; it does not
    /// emit the loads).
    ///
    /// Wait — the caller loads each slot, then the drop consumes it.
    /// See the implementation for the exact protocol.
    static void emitScopeDrops(
        compile::CompilerContext& ctx,
        const compile::ScopeRecord& scope);

    /// Emit drops for every resource-typed slot in every open scope
    /// except the value on top of stack (the return value). Called
    /// before a `return` statement.
    static void emitReturnDrops(compile::CompilerContext& ctx);

    /// Emit drops for every resource-typed slot in every open scope
    /// up to (but not including) the target loop's body. Called
    /// before a `break` or `continue`.
    ///
    /// `targetScopeIndex` is the index of the target loop's body
    /// scope in the slot allocator's scope stack. The slots of
    /// scopes with index >= targetScopeIndex are dropped.
    static void emitLoopExitDrops(compile::CompilerContext& ctx,
                                  size_t targetScopeIndex);
};

} // namespace lucid::bytecode::memory