/**
 * @file interp/InterpreterConfig.hpp
 *
 * @responsibility Options that affect how the interpreter runs a
 *                 program: the panic policy, resource limits, and the
 *                 debug-mode invariants.
 *
 * ─── Design: config is a value, not global state ──────────────────────────
 * Two interpreters in the same process can have different configs.
 * The config is passed to the Interpreter's constructor and stored by
 * value.
 *
 * ─── Dependencies ─────────────────────────────────────────────────────────
 * None.
 */

#pragma once

#include <cstdint>

namespace lucid::interp {

struct InterpreterConfig {
    /// The maximum Lucid call depth. A call that would exceed this
    /// raises Panic_StackOverflow (7102). Default is 1024, which is
    /// deep enough for normal recursive code and shallow enough that
    /// a runaway recursion is caught before the C++ stack is exhausted.
    uint32_t maxCallDepth = 1024;

    /// The maximum number of Values in a single operand stack. The
    /// compiler's stack-depth tracking should keep every function
    /// under this; the interpreter checks it defensively.
    uint32_t maxOperandStack = 65536;

    /// If true, the interpreter asserts its own invariants after every
    /// opcode: the operand stack depth matches what the compiler
    /// expected, no slot holds Uninitialized, etc. This is slow and
    /// is intended for test builds only. Default false.
    bool debugInvariants = false;

    /// If true, a panic prints a human-readable trace to stderr before
    /// being converted into an ExecutionResult. Intended for
    /// development; the host should log the returned panic instead in
    /// production. Default false.
    bool printPanicsToStderr = false;
};

} // namespace lucid::interp