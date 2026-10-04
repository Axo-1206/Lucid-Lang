/**
 * @file runtime/Panic.hpp
 *
 * @responsibility The panic value — the runtime-side failure type.
 *                 A panic carries a DiagCode (the same code space the
 *                 compiler uses for diagnostics), a pre-formatted
 *                 message, and a Lucid stack trace.
 *
 * ─── Design: a runtime concept, not a compiler one ────────────────────────
 * A panic is raised only at runtime: by the interpreter (a nil
 * dereference, an out-of-bounds index, a stale reference, a duplicate
 * @primary key, a divide-by-zero in a non-constant expression) or by
 * the runtime library (an allocation failure, generation exhaustion, a
 * host call that returned an error). It unwinds to the host call
 * boundary (grammar §10).
 *
 * The compiler does NOT raise panics. Sema reports diagnostics through
 * DiagnosticEngine, which has its own Severity scale (Hint..Fatal),
 * its own SourceLocation, and its own recovery semantics. The two
 * channels share a DiagCode vocabulary (DiagCode.hpp's "codes describe
 * what, not when") but not a type. A Panic never appears at compile
 * time.
 *
 * ─── Design: no C++ exceptions in this header ─────────────────────────────
 * Panic is a value. The C++ exception type that carries it
 * (interp::PanicException) lives in interp/InterpreterError.hpp.
 * Panic.hpp does not define or throw anything.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * core/diagnostics/DiagCode.hpp.
 */

#pragma once

#include "core/diagnostics/DiagCode.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lucid::runtime {

/// @brief One captured Lucid stack frame.
///
/// Produced by the interpreter when a panic is raised; consumed by the
/// host's error logger. A frame names the function and the source
/// location the interpreter was executing when the panic fired.
struct StackFrame {
    /// The mangled name of the function. Empty if the function is a
    /// compiler-generated lambda or an anonymous frame.
    std::string function;

    /// The module the function belongs to. Empty if unknown.
    std::string module;

    /// The source line, or 0 if the line table had no entry for this
    /// ip.
    uint32_t line = 0;

    /// The source column, or 0 if unavailable.
    uint32_t column = 0;
};

/// @brief A panic raised by the runtime or the interpreter.
///
/// A panic is not recoverable inside Lucid (there is no try/catch in
/// the grammar, §10). It unwinds to the host call boundary. The host
/// decides what to do with it.
struct Panic {
    /// The diagnostic code describing what went wrong. Always a code
    /// from the Panic_*, Value_*, Table_*, or Mem_* bands.
    diag::DiagCode code;

    /// The pre-formatted message. Human-readable, includes any
    /// context the raiser had (a column name, a value, an index).
    std::string message;

    /// The Lucid call stack at the moment the panic fired. Outermost
    /// frame first. May be empty if the panic was raised by the
    /// loader before any function was entered.
    std::vector<StackFrame> stack;

    /// Convenience: true if this panic carries a stack trace.
    bool hasStack() const noexcept { return !stack.empty(); }
};

} // namespace lucid::runtime