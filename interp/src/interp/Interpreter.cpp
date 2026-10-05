/**
 * @file interp/Interpreter.cpp
 *
 * @responsibility The interpreter's public facade: call a function by
 *                 name, catch panics, return an ExecutionResult.
 *
 * ─── Design: the boundary converts exceptions to results ──────────────────
 * The interpreter's internals throw PanicException. The host-facing
 * API returns ExecutionResult, which carries a Panic as a value.
 * This file is where the two meet: call() wraps the internal run in
 * a try/catch and produces the result variant.
 *
 * ─── Design: no state beyond the Impl ─────────────────────────────────────
 * The public Interpreter holds a LoadedProgram reference, a config,
 * and the internal state. It does not itself interpret anything. Its
 * only job is to translate a name to a function index, run the
 * function, and convert the outcome.
 */

#include "interp/Interpreter.hpp"

#include "interp/FunctionRef.hpp"
#include "interp/InterpreterError.hpp"
#include "interp/InterpreterInternal.hpp"
#include "interp/LoadedProgram.hpp"

#include "bytecode/Bytecode.hpp"

#include "core/diagnostics/DiagCode.hpp"

#include "runtime/Exceptions.hpp"
#include "runtime/Panic.hpp"
#include "runtime/Value.hpp"

#include <cassert>
#include <string>
#include <utility>

namespace lucid::interp {

// ─────────────────────────────────────────────────────────────────────────
// Interpreter::Impl
// ─────────────────────────────────────────────────────────────────────────
//
// The public Interpreter's opaque implementation. It holds the
// InterpreterInternal (frame stack, run loop state).

struct Interpreter::Impl {
    InterpreterInternal internal;

    Impl(LoadedProgram& program, const InterpreterConfig& config)
        : internal(program, config) {}
};

// ─────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────

Interpreter::Interpreter(LoadedProgram& program, InterpreterConfig config)
    : m_program(program)
    , m_config(config)
    , m_impl(std::make_unique<Impl>(program, m_config))
{
}

Interpreter::~Interpreter() = default;

Interpreter::Interpreter(Interpreter&&) noexcept = default;

// A reference member (LoadedProgram& m_program) cannot be
// reseated, so no move-assignment operator is defined.

// ─────────────────────────────────────────────────────────────────────────
// call
// ─────────────────────────────────────────────────────────────────────────

ExecutionResult Interpreter::call(std::string_view name,
                                  const Value* args,
                                  uint32_t argCount) {
    // ─── Resolve the name ─────────────────────────────────────────────
    //
    // The Bytecode's function index maps a mangled name to a function
    // index. A name that doesn't resolve is not a panic the script
    // caused; it's a host error (calling a name that doesn't exist).
    // It becomes a Name_UndefinedValue error, wrapped as a Panic.
    const bytecode::Bytecode* code = m_program.bytecode();
    if (!code) {
        return ExecutionResult::panicked(runtime::Panic{
            diag::DiagCode::Bc_DeserializationFailed,
            "interpreter called on a program with no Bytecode",
            {}
        });
    }

    const auto functionIndex = code->findFunction(name);
    if (!functionIndex) {
        return ExecutionResult::panicked(runtime::Panic{
            diag::DiagCode::Name_UndefinedValue,
            "no exported function named '" + std::string(name) + "'",
            {}
        });
    }

    // ─── Run ──────────────────────────────────────────────────────────
    //
    // The dispatch loop may throw PanicException (a script-level
    // panic) or runtime::RuntimeException (a runtime-level failure
    // the interpreter didn't wrap). Both convert to an
    // ExecutionResult::Panicked.
    try {
        runtime::Value result =
            m_impl->internal.run(*functionIndex, args, argCount);
        return ExecutionResult::completed(result);
    }
    catch (const PanicException& e) {
        return ExecutionResult::panicked(e.panic());
    }
    catch (const runtime::RuntimeException& e) {
        // A runtime-level failure that reached this boundary without
        // being re-wrapped. In practice, the interpreter's handlers
        // catch RuntimeException at the opcode level and re-throw as
        // PanicException, so this is defensive. The stack trace is
        // empty (the runtime has no Lucid stack to capture).
        return ExecutionResult::panicked(runtime::Panic{
            e.code(),
            e.what(),
            {}
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────
// tick
// ─────────────────────────────────────────────────────────────────────────

void Interpreter::tick(double /*dt*/) {
    // Sequences are deferred (grammar §9.2, §13.1). v1 has no
    // scheduler and no coroutine list. This method exists so the
    // host's per-frame loop does not need to change when sequences
    // land.
}

} // namespace lucid::interp