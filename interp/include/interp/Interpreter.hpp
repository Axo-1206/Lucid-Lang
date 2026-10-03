/**
 * @file interp/Interpreter.hpp
 *
 * @responsibility The interpreter's public API: load a program, call
 *                 a function, tick the scheduler (reserved for v2).
 *
 * ─── Design: the loader and the runner are separate objects ───────────────
 * Loader produces a LoadedProgram. Interpreter consumes a
 * LoadedProgram. This split matters because the loader needs the
 * host registry and the runtime; the interpreter's hot loop needs
 * neither. A host can load a program once and run it many times
 * without touching the registry again.
 *
 * ─── Design: the interpreter is not thread-safe ───────────────────────────
 * One Interpreter runs one LoadedProgram on one thread. A host that
 * wants concurrency runs multiple Interpreters on multiple programs,
 * each on its own thread. This mirrors the grammar's "cooperative,
 * single-threaded" model (§9.2.6) — there are no threads inside Lucid.
 *
 * ─── Design: call by name, not by index ───────────────────────────────────
 * The host calls an @export'ed function by its name (the mangled name
 * the compiler assigned). The interpreter looks it up in the
 * Bytecode's function index and dispatches. There is no
 * call-by-index API — the host is not expected to know the index.
 */

#pragma once

#include "ExecutionResult.hpp"
#include "LoadedProgram.hpp"
#include "interp/ExecutionResult.hpp"
#include "interp/InterpreterConfig.hpp"
#include "interp/LoadedProgram.hpp"
#include "interp/Value.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace lucid::interp {

/// @brief The interpreter.
///
/// Constructed once per program; runs one or more top-level calls.
/// Not thread-safe: a host that wants concurrent Lucid execution runs
/// one Interpreter per thread, each on its own LoadedProgram.
class Interpreter {
public:
    /// Construct an interpreter over a loaded program. The
    /// LoadedProgram must outlive the Interpreter (the interpreter
    /// holds a reference, not a copy).
    Interpreter(LoadedProgram& program, InterpreterConfig config = {});

    ~Interpreter();

    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;
    Interpreter(Interpreter&&) noexcept;
    Interpreter& operator=(Interpreter&&) noexcept;

    // ─── Calling ────────────────────────────────────────────────────────

    /// Call an @export'ed function by name with the given arguments.
    ///
    /// The name is the mangled name the compiler assigned to the
    /// function. A name that does not resolve to an @export'ed
    /// function returns an ExecutionResult::Panicked with a
    /// Name_UndefinedValue code (the same code the compiler would
    /// have used for an undefined name).
    ///
    /// On completion, the result is ExecutionResult::Completed and
    /// carries the function's return value (a Nil Value for a void
    /// function).
    ///
    /// On panic, the result is ExecutionResult::Panicked and carries
    /// the Panic (code, message, stack trace). The interpreter's
    /// frames have been unwound to the host boundary; the interpreter
    /// is ready for the next call.
    ///
    /// v1 never returns ExecutionResult::Suspended (sequences are
    /// rejected by Sema).
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

    /// The frame stack. The interpreter maintains it; the host never
    /// sees it. Its lifetime is the Interpreter's.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace lucid::interp