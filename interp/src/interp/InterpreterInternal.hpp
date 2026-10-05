/**
 * @file interp/InterpreterInternal.hpp
 *
 * @responsibility The interpreter's internal execution state and the
 *                 shared context the dispatch loop and opcode
 *                 handlers use.
 *
 * ─── Design: named type, not a nested Impl ────────────────────────────────
 * The public Interpreter's `struct Impl; std::unique_ptr<Impl>` is a
 * forward declaration; this header names the concrete type. The
 * dispatch loop (`Dispatch.cpp`) and the opcode handlers
 * (`Ops/ *.cpp`) take an `InterpreterInternal&`.
 *
 * ─── Design: the frame stack is the interpreter's state ───────────────────
 * The interpreter is a stack machine. Its state is: the frames
 * currently pushed, in outermost-first order, plus the topmost frame's
 * `ip` and operand stack (which the Frame itself owns). Everything
 * else — the program, the config — is borrowed.
 *
 * ─── Design: run() is the entry, not the loop ─────────────────────────────
 * `run()` pushes a frame for the target function, sets its parameters
 * from the caller's arguments, and calls `dispatch(*this)`. The
 * dispatch loop itself lives in Dispatch.cpp; this class owns the
 * frame stack and the entry/exit logic.
 *
 * ─── Design: maxCallDepth is enforced here, not in the dispatcher ─────────
 * A call pushes a frame; if the push would exceed the configured
 * maximum, the push raises a Panic_StackOverflow. Enforcing it in
 * one place (pushFrame) keeps the dispatcher's Ext_Call case simple
 * and consistent.
 *
 * ─── Design: a Frame& is invalidated by the next pushFrame ────────────────
 * pushFrame appends to the frame vector, which may reallocate. Any
 * reference to a frame obtained before a push is dangling after it.
 * A handler that pushes a frame must use the reference returned by
 * pushFrame, not a previously held one.
 *
 * ─── Design: the top-level return value is captured here ──────────────────
 * The dispatcher's loop exits when the frame stack empties. The
 * return value of the last frame is stashed here by opsControl's
 * Ext_Return/Ext_ReturnVoid handler and read by the dispatcher after
 * the loop. A panic never sets it; the host-call boundary catches
 * the exception before reading it.
 */

#pragma once

#include "interp/Frame.hpp"
#include "interp/InterpreterConfig.hpp"
#include "interp/LoadedProgram.hpp"

#include "runtime/Value.hpp"

#include <cstdint>
#include <vector>

namespace lucid::interp {

class InterpreterInternal;

/// @brief The interpreter's main loop. Defined in Dispatch.cpp.
///
/// Runs until the frame stack empties. Returns the top-level frame's
/// return value. Throws PanicException on a panic.
runtime::Value dispatch(InterpreterInternal& interp);

/// @brief The interpreter's internal execution state.
class InterpreterInternal {
public:
    InterpreterInternal(LoadedProgram& program,
                        const InterpreterConfig& config);

    InterpreterInternal(const InterpreterInternal&) = delete;
    InterpreterInternal& operator=(const InterpreterInternal&) = delete;
    InterpreterInternal(InterpreterInternal&&) = delete;
    InterpreterInternal& operator=(InterpreterInternal&&) = delete;
    ~InterpreterInternal() = default;

    // ─── Borrowed context ───────────────────────────────────────────────

    LoadedProgram& program() noexcept { return m_program; }
    const LoadedProgram& program() const noexcept { return m_program; }

    const InterpreterConfig& config() const noexcept { return m_config; }

    // ─── Frame stack ────────────────────────────────────────────────────

    std::vector<Frame>& frames() noexcept { return m_frames; }
    const std::vector<Frame>& frames() const noexcept { return m_frames; }

    /// The currently executing frame. Precondition: frames() is
    /// non-empty. INVALIDATED by the next pushFrame call.
    Frame& currentFrame() noexcept { return m_frames.back(); }
    const Frame& currentFrame() const noexcept { return m_frames.back(); }

    /// Push a frame for the given function. Raises Panic_StackOverflow
    /// if the new depth would exceed the configured maximum. Returns
    /// a reference to the new frame; any previously held Frame& is
    /// invalid after this call.
    Frame& pushFrame(const bytecode::FunctionProto* proto);

    /// Pop the top frame. Precondition: frames() is non-empty.
    void popFrame() noexcept { m_frames.pop_back(); }

    uint32_t callDepth() const noexcept {
        return static_cast<uint32_t>(m_frames.size());
    }

    // ─── Top-level result ───────────────────────────────────────────────

    /// Set the top-level frame's return value. Called by opsControl's
    /// Ext_Return / Ext_ReturnVoid handler when the frame stack is
    /// about to become empty.
    void setTopLevelResult(runtime::Value v) {
        m_topLevelResult = std::move(v);
    }

    /// Read the top-level result. Called by the dispatcher after the
    /// frame stack empties.
    runtime::Value takeTopLevelResult() {
        return std::move(m_topLevelResult);
    }

    // ─── Run ────────────────────────────────────────────────────────────

    /// Call the function at `functionIndex` with the given arguments
    /// and run it to completion. Precondition: the frame stack is
    /// empty (not reentrant). Throws PanicException on a panic.
    runtime::Value run(uint32_t functionIndex,
                       const runtime::Value* args,
                       uint32_t argCount);

private:
    LoadedProgram& m_program;
    const InterpreterConfig& m_config;
    std::vector<Frame> m_frames;
    runtime::Value m_topLevelResult;
};

} // namespace lucid::interp