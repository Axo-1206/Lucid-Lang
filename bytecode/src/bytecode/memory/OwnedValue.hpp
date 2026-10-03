/**
 * @file bytecode/memory/OwnedValue.hpp
 *
 * @responsibility A parallel stack of ownership flags, one per value
 *                 on the value stack. Every value-producing operation
 *                 pushes a flag; every value-consuming operation pops
 *                 one. The flag says whether the value owns a resource
 *                 that must be dropped, or whether its resource was
 *                 transferred to a consumer.
 *
 * ─── Why the flag is needed ───────────────────────────────────────────────
 * A `string` value on the value stack owns a heap buffer. The emitter
 * has to know, at every point where the value goes out of scope or is
 * overwritten, whether to emit a drop. The type tells the emitter
 * *that* the value might own something; the flag tells it whether the
 * specific value at this point does own something (vs. having been
 * moved out already).
 *
 * ─── The protocol ─────────────────────────────────────────────────────────
 * The emitter follows this protocol:
 *
 *   1. After emitting an expression, call pushOwned() or
 *      pushBitCopy() to record the value's ownership.
 *
 *   2. Before consuming a value (a store, a call argument, a drop),
 *      call pop() to read its ownership flag. The emitter uses the
 *      flag to decide whether to emit a drop or a retain.
 *
 *   3. When a value is moved out (the callee takes ownership), call
 *      markTopAsMoved() before the consuming operation.
 *
 * ─── Design: the stack is not the value stack ─────────────────────────────
 * OwnedValueStack is a *bookkeeping* stack, parallel to the interpreter's
 * value stack. It exists only at compile time. It has no relation to the
 * FunctionProto or the artifact. Its whole purpose is to let the emitter
 * answer "does the value on top own a resource?" at each point.
 *
 * ─── Design: matching the value stack's shape ─────────────────────────────
 * The invariant is: at every code point, OwnedValueStack::size() ==
 * CompilerContext::currentDepth(). Every emitOpcode that changes the
 * value-stack depth must be followed by a corresponding push or pop
 * on OwnedValueStack. In practice, the emitter pushes/pops as it
 * consumes and produces values; the two stacks stay in sync.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace lucid::bytecode::memory {

/// @brief The ownership state of one value on the value stack.
enum class Ownership : uint8_t {
    /// The value owns its resources. If it is discarded, dropped, or
    /// overwritten, the emitter must emit a drop for it.
    Owned,

    /// The value does not own any resource (a primitive, a row
    /// reference, a function value, or a value whose resource was
    /// already moved out). Dropping it is a no-op.
    BitCopy,

    /// The value's resource was transferred to a consumer. The value
    /// is still on the stack but must NOT be dropped; the consumer
    /// owns it now.
    Moved,
};

class OwnedValueStack {
public:
    OwnedValueStack() = default;

    // ─── Pushing values ────────────────────────────────────────────────

    /// Record that a value was pushed onto the value stack, and that
    /// it owns its resources (if any). Called by the emitter after
    /// producing a value that hasn't been transferred.
    void pushOwned() { m_stack.push_back(Ownership::Owned); }

    /// Record that a value was pushed, and that it owns nothing.
    /// Called after producing a primitive, a row reference, or a
    /// function value.
    void pushBitCopy() { m_stack.push_back(Ownership::BitCopy); }

    /// Record that a value was pushed but its resource was already
    /// transferred. Rarely used directly; callers usually pushOwned
    /// then markTopAsMoved.
    void pushMoved() { m_stack.push_back(Ownership::Moved); }

    /// Push `n` entries at once. Used after opcodes that push multiple
    /// values (Ext_Dup, Ext_Copy).
    void pushN(uint32_t n, Ownership state) {
        m_stack.insert(m_stack.end(), n, state);
    }

    // ─── Popping values ────────────────────────────────────────────────

    /// Read and remove the top value's ownership. Precondition: the
    /// stack is non-empty.
    Ownership pop();

    /// Read the top value's ownership without removing it.
    /// Precondition: the stack is non-empty.
    Ownership peek() const;

    /// Peek at the nth value from the top (0 = top). Precondition:
    /// n < size().
    Ownership peekAt(size_t n) const;

    // ─── Mutating the top ──────────────────────────────────────────────

    /// Mark the top value as moved. The value stays on the stack but
    /// will not be dropped; its resource belongs to a consumer now.
    void markTopAsMoved();

    // ─── Shape ─────────────────────────────────────────────────────────

    size_t size() const noexcept { return m_stack.size(); }
    bool empty() const noexcept { return m_stack.empty(); }

    /// Assert that the stack is empty. Called by CompilerContext::
    /// finalizeProto to verify that every value produced during the
    /// function's emission was consumed.
    void assertEmpty() const;

private:
    std::vector<Ownership> m_stack;
};

} // namespace lucid::bytecode::memory