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
 * The ownership stack is kept in sync with the value stack
 * automatically. CompilerContext::emitOpcode and noteStackEffect push
 * one BitCopy entry for every value an opcode produces, and pop one
 * entry for every value it consumes. The emitter never pushes or pops
 * for the count.
 *
 * The emitter intervenes only to say "this produced value owns a
 * resource." After emitting an opcode that produces a resource-owning
 * value, the emitter calls markTopAsOwned(). Every other produced
 * value keeps its default BitCopy state.
 *
 * A value whose resource is transferred to a consumer (a return value,
 * a call argument) is marked Moved via markTopAsMoved().
 *
 * ─── Design: the stack is not the value stack ─────────────────────────────
 * OwnedValueStack is a *bookkeeping* stack, parallel to the interpreter's
 * value stack. It exists only at compile time. It has no relation to the
 * FunctionProto or the artifact. Its whole purpose is to let the emitter
 * answer "does the value on top own a resource?" at each point.
 *
 * ─── Design: matching the value stack's shape ─────────────────────────────
 * The invariant "OwnedValueStack::size() == CompilerContext::currentDepth()"
 * is enforced by construction: CompilerContext::emitOpcode and
 * noteStackEffect are the only code paths that change the size, and they
 * change it by exactly the amount the value stack changed. Emitters add
 * entries only via markTopAsOwned and markTopAsMoved, both of which
 * replace the state of an existing entry rather than changing the size.
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

    /// Push `n` entries at once. Used by the auto-bookkeeping when an
    /// opcode's OpcodeInfo reports `pushes > 1` (the special-cased
    /// `Ext_Dup`, or an opcode whose operand resolves to a multi-push
    /// effect via noteStackEffect).
    ///
    /// Emitters do not call this directly; the mechanism that does is
    /// CompilerContext::emitOpcode. If a new emitter ever needs to
    /// push more than one entry for a single value-producing call, it
    /// should extend the auto-bookkeeping rather than reach for this
    /// method.
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

    /// Set the top entry to Owned. Called by the emitter after
    /// producing a value whose type owns a resource.
    ///
    /// The auto-bookkeeping in CompilerContext::emitOpcode and
    /// noteStackEffect pushes one BitCopy entry for every value an
    /// opcode produces. This method overrides the top entry when the
    /// produced value is resource-owning. It does not push; it
    /// replaces.
    ///
    /// Precondition: the stack is non-empty. The auto-bookkeeping
    /// guarantees an entry exists for every value on the value stack,
    /// so a caller that has just emitted a value-producing opcode can
    /// rely on the top entry being present.
    void markTopAsOwned();

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