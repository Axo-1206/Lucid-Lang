/**
 * @file interp/Interpreter.hpp
 *
 * @responsibility The interpreter's public API: load a program, call
 *                 a function, tick the scheduler (reserved for v2).
 *
 * ─── Design: the loader and the runner are separate objects ───────────────
 * Loader produces a LoadedProgram. Interpreter consumes a
 * LoadedProgram. This split matters because the loader needs the host
 * registry and the runtime; the interpreter's hot loop needs neither.
 *
 * ─── Design: the interpreter is not thread-safe ───────────────────────────
 * One Interpreter runs one LoadedProgram on one thread.
 *
 * ─── Design: call by name, not by index ───────────────────────────────────
 * The host calls an @export'ed function by its mangled name.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * interp/ExecutionResult.hpp, interp/InterpreterConfig.hpp,
 * interp/LoadedProgram.hpp, runtime/Value.hpp.
 */

#pragma once

#include "interp/ExecutionResult.hpp"
#include "interp/InterpreterConfig.hpp"
#include "interp/LoadedProgram.hpp"
#include "runtime/Value.hpp"

#include <memory>
#include <string_view>

namespace lucid::interp {

using lucid::runtime::Value;

/// @brief The interpreter.
class Interpreter {
public:
    /// Construct an interpreter over a loaded program. The
    /// LoadedProgram must outlive the Interpreter.
    Interpreter(LoadedProgram& program, InterpreterConfig config = {});

    ~Interpreter();

    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;
    Interpreter(Interpreter&&) noexcept;
    Interpreter& operator=(Interpreter&&) noexcept;

    // ─── Calling ────────────────────────────────────────────────────────

    /// Call an @export'ed function by name with the given arguments.
    ///
    /// On completion, the result is ExecutionResult::Completed.
    /// On panic, the result is ExecutionResult::Panicked. v1 never
    /// returns ExecutionResult::Suspended.
    ExecutionResult call(std::string_view name,
                         const Value* args, uint32_t argCount);

    /// Overload for a no-argument call.
    ExecutionResult call(std::string_view name) {
        return call(name, nullptr, 0);
    }

    // ─── Scheduler (reserved for v2) ────────────────────────────────────

    /// Advance every polled sequence by one tick. v1 has no sequences;
    /// this method exists so the host's per-frame loop does not need
    /// to change when sequences land. Calling it in v1 is a no-op.
    void tick(double dt);

    // ─── Accessors ──────────────────────────────────────────────────────

    LoadedProgram& program() noexcept { return m_program; }
    const InterpreterConfig& config() const noexcept { return m_config; }

private:
    LoadedProgram& m_program;
    InterpreterConfig m_config;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lucid::interp