/**
 * @file interp/InterpreterError.hpp
 *
 * @responsibility The C++ exception type the interpreter throws when a
 *                 panic is raised. Carries a runtime::Panic value. The
 *                 host-call boundary catches it and converts it into
 *                 an ExecutionResult::Panicked.
 *
 * ─── Design: exceptions for the panic path, not the happy path ────────────
 * A panic is a bug in the Lucid program — a nil dereference, an
 * out-of-bounds index, a duplicate primary key. It is rare. The
 * interpreter's main dispatch loop pays nothing on the happy path (no
 * status word to check); the panic path pays the cost of a C++
 * exception unwind, which is fine because it happens approximately
 * never.
 *
 * ─── Design: no drops on unwind ───────────────────────────────────────────
 * When a PanicException unwinds a Lucid frame, the frame's local
 * resources (strings, host handles held in local slots) are NOT
 * dropped. The compiled code's drop schedule only runs on normal
 * control flow. This is a documented v1 limitation (grammar §10
 * discussion of panic). The host is expected to recover the
 * entity/context, not the frame's memory.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * runtime/Panic.hpp.
 */

#pragma once

#include "runtime/Panic.hpp"

#include <exception>
#include <string>
#include <utility>

namespace lucid::interp {

/// @brief The exception the interpreter throws on a panic.
class PanicException : public std::exception {
public:
    explicit PanicException(runtime::Panic p)
        : m_panic(std::move(p)) {}

    PanicException(diag::DiagCode code, std::string message)
        : m_panic{code, std::move(message), {}} {}

    /// The structured panic value. The host-call boundary moves this
    /// out into an ExecutionResult::Panicked.
    const runtime::Panic& panic() const noexcept { return m_panic; }

    runtime::Panic&& takePanic() noexcept { return std::move(m_panic); }

    const char* what() const noexcept override {
        return m_panic.message.c_str();
    }

private:
    runtime::Panic m_panic;
};

} // namespace lucid::interp