/**
 * @file runtime/Exceptions.hpp
 *
 * @responsibility The runtime library's failure types. A runtime
 *                 operation that can fail on a language-level rule
 *                 throws one of these; the interpreter catches it at
 *                 the opcode handler and re-wraps it as a
 *                 PanicException.
 *
 * ─── Design: a small, closed set ──────────────────────────────────────────
 * Only two runtime operations can fail on a script-visible rule:
 * TableObject::addRow and TableObject::setCell. Both check for
 * duplicate keys; addRow also checks for generation exhaustion. No
 * other runtime operation throws. The types here are the complete set.
 *
 * ─── Design: the code matches the panic code space ────────────────────────
 * Each exception carries a DiagCode from the Panic_* band
 * (Panic_DuplicateKey, Panic_GenerationExhausted). When the
 * interpreter catches one, it converts to a runtime::Panic with the
 * same code and message, then throws a PanicException. The user sees
 * a panic with the code the grammar documents (§7.3, §4.1.1a).
 *
 * ─── Design: std::exception derived ───────────────────────────────────────
 * The base derives from std::exception so a generic catch site (a
 * host's logging wrapper, a test's RAII guard) can catch it without
 * knowing the specific type. The concrete types are the ones a
 * specific handler would catch by name.
 *
 * ─── Design: no dependency on runtime::Panic ──────────────────────────────
 * A RuntimeException carries a DiagCode and a message, not a full
 * Panic. A Panic carries a Lucid stack trace; the runtime does not
 * have a stack to attach. The interpreter adds the stack when it
 * re-wraps.
 */

#pragma once

#include "core/diagnostics/DiagCode.hpp"

#include <exception>
#include <string>
#include <utility>

namespace lucid::runtime {

/// @brief The base class for every runtime-level exception.
///
/// A RuntimeException is raised by a runtime operation that can fail
/// on a language-level rule. It carries the DiagCode the grammar
/// documents for that failure. It does not carry a Lucid stack trace
/// (the runtime has no call stack of its own); the interpreter adds
/// the trace when it converts to a Panic.
class RuntimeException : public std::exception {
public:
    RuntimeException(diag::DiagCode code, std::string message)
        : m_code(code), m_message(std::move(message)) {}

    ~RuntimeException() override;   // key function; defined in .cpp

    /// The diagnostic code, from the Panic_* band.
    diag::DiagCode code() const noexcept { return m_code; }

    /// The message, for the host's log.
    const char* what() const noexcept override { return m_message.c_str(); }

    /// The message as a string, for building a Panic.
    const std::string& message() const noexcept { return m_message; }

private:
    diag::DiagCode m_code;
    std::string    m_message;
};

/// @brief A duplicate key in a @unique or @primary column.
///
/// Raised by TableObject::addRow when a new row's key already exists,
/// and by TableObject::setCell when a cell write would create a
/// duplicate. Grammar §7.3, §4.1.5.
class DuplicateKeyError : public RuntimeException {
public:
    DuplicateKeyError()
        : RuntimeException(diag::DiagCode::Panic_DuplicateKey,
                           "duplicate @unique or @primary value") {}
};

/// @brief A growing table's generation counter is exhausted.
///
/// Raised by TableObject::addRow when the table's monotonic
/// generation counter reaches its limit. Grammar §4.1.1a: "If a
/// table's generation counter is exhausted, the next ADD panics
/// rather than wrap around."
class GenerationExhaustedError : public RuntimeException {
public:
    GenerationExhaustedError()
        : RuntimeException(diag::DiagCode::Panic_GenerationExhausted,
                           "table generation counter exhausted") {}
};

} // namespace lucid::runtime