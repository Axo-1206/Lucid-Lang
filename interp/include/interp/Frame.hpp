/**
 * @file interp/Frame.hpp
 *
 * @responsibility One Lucid call frame: the local slots, the operand
 *                 stack, the instruction pointer, and a pointer to the
 *                 function being executed.
 *
 * ─── Design: per-frame operand stack ──────────────────────────────────────
 * Each frame owns its operand stack, sized at frame construction from
 * the function's maxStackDepth. The reason is sequences: a @sequence
 * frame must be freezable (all its live state saved and restored
 * later), and a frame with its own operand stack is a single
 * contiguous blob that can be memcpy'd.
 *
 * ─── Design: locals are slots, indexed by u16 ─────────────────────────────
 * LoadLocal/StoreLocal take a u16 slot operand. The frame's locals
 * array is indexed by that slot. Parameters occupy slots
 * [0, paramCount) and are filled by the caller; locals occupy the
 * rest. The compiler's SlotAllocator assigned those slots.
 *
 * ─── Design: the frame does not own its FunctionProto ─────────────────────
 * The FunctionProto lives in the LoadedProgram and outlives every
 * frame. The frame holds a pointer.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * runtime/Value.hpp. FunctionProto is forward-declared; the .cpp
 * includes the definition.
 */

#pragma once

#include "runtime/Value.hpp"

#include <cstdint>
#include <vector>

namespace lucid::bytecode {
    struct FunctionProto;
}

namespace lucid::interp {

using lucid::runtime::Value;

/// @brief One Lucid call frame.
///
/// Constructed by the interpreter when a call is made; destroyed when
/// the function returns or panics. A frame is not copyable; it is
/// movable so a vector of frames can grow without invalidating
/// references.
class Frame {
public:
    /// Construct a frame for the given function. Allocates localSlots
    /// Values and maxStackDepth Values for the operand stack. The
    /// frame's ip starts at 0; the caller sets it if resuming.
    Frame(const bytecode::FunctionProto* proto, uint32_t callDepth);

    Frame(Frame&&) noexcept = default;
    Frame& operator=(Frame&&) noexcept = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    ~Frame() = default;

    // ─── Accessors ──────────────────────────────────────────────────────

    const bytecode::FunctionProto* proto() const noexcept { return m_proto; }
    uint32_t callDepth() const noexcept { return m_callDepth; }

    /// The instruction pointer: an offset into the proto's code bytes.
    uint32_t ip() const noexcept { return m_ip; }
    void setIp(uint32_t ip) noexcept { m_ip = ip; }
    void advanceIp(uint32_t n) noexcept { m_ip += n; }

    // ─── Local slots ────────────────────────────────────────────────────

    /// Read a local slot. Precondition: slot < localCount().
    const Value& local(uint32_t slot) const noexcept { return m_locals[slot]; }
    Value& local(uint32_t slot) noexcept { return m_locals[slot]; }

    uint32_t localCount() const noexcept {
        return static_cast<uint32_t>(m_locals.size());
    }

    /// The first `n` slots are parameters, filled by the caller before
    /// the function body runs.
    void setParameter(uint32_t index, Value v) noexcept {
        m_locals[index] = v;
    }

    // ─── Operand stack ──────────────────────────────────────────────────

    /// Push a value onto the operand stack. Precondition: depth() <
    /// maxDepth().
    void push(Value v) noexcept {
        m_stack[m_sp++] = v;
    }

    /// Pop the top of the operand stack. Precondition: depth() > 0.
    Value pop() noexcept {
        return m_stack[--m_sp];
    }

    /// Peek at the top of the operand stack without popping.
    /// Precondition: depth() > 0.
    const Value& top() const noexcept { return m_stack[m_sp - 1]; }
    Value& top() noexcept { return m_stack[m_sp - 1]; }

    /// Peek at the value `n` slots below the top (0 == top).
    const Value& peek(uint32_t n) const noexcept {
        return m_stack[m_sp - 1 - n];
    }
    Value& peek(uint32_t n) noexcept {
        return m_stack[m_sp - 1 - n];
    }

    /// The current operand stack depth.
    uint32_t depth() const noexcept { return m_sp; }

    /// The maximum operand stack depth (the function's maxStackDepth).
    uint32_t maxDepth() const noexcept {
        return static_cast<uint32_t>(m_stack.size());
    }

    /// Direct access to the operand stack, for aggregate opcodes that
    /// need to read several values at once.
    Value* stackData() noexcept { return m_stack.data(); }
    const Value* stackData() const noexcept { return m_stack.data(); }

private:
    const bytecode::FunctionProto* m_proto = nullptr;
    uint32_t m_callDepth = 0;
    uint32_t m_ip = 0;
    uint32_t m_sp = 0;

    std::vector<Value> m_locals;
    std::vector<Value> m_stack;
};

} // namespace lucid::interp