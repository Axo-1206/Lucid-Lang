/// @file support/ExecutionResult.hpp
/// @brief Result of executing a module.

#pragma once

#include <string>

namespace interpreter {

/// @brief Result of executing a program.
///
/// ─── Field Semantics ─────────────────────────────────────────────────
/// The interpreter reports two distinct kinds of outcome through this
/// struct, and callers must not conflate them:
///
///   Interpreter-level failure — the entry point could not be reached
///   or invoked. Reported via `success = false` and a non-empty
///   `errorMessage`. `exitCode` is set to 1 as a conventional failure
///   sentinel, but it is not meaningful (the program never ran).
///
///   Program-level result — the entry point ran to completion.
///   `exitCode` is the value it returned. `success` is set to
///   `(exitCode == 0)`, matching the conventional C `main` contract.
///   `errorMessage` is empty.
///
/// Both kinds can be observed by a caller; which one it's looking at is
/// determined by whether `errorMessage` is empty.
struct ExecutionResult {
    /// Exit code from the entry point (0 = success).
    ///
    /// On interpreter-level failure this is set to 1 as a sentinel and
    /// should be ignored. On program-level completion it is the actual
    /// value returned by the entry point function.
    int exitCode = 0;

    /// Whether the run succeeded.
    ///
    /// true  — the entry point ran and returned 0.
    /// false — either the entry point ran and returned non-zero, or the
    ///         interpreter could not invoke it (see `errorMessage`).
    bool success = true;

    /// Error message if execution failed at the interpreter level.
    ///
    /// Empty when the entry point ran (regardless of its exit code).
    /// Non-empty when the interpreter could not reach or invoke the
    /// entry point, in which case `success` is false.
    std::string errorMessage;

    /// Execution time in milliseconds. Set only when the entry point ran.
    double executionTimeMs = 0.0;

    /// The entry point that was executed. Set only when the entry point
    /// ran. Contains the JIT symbol name (which equals the source name
    /// for @[export]ed functions — see MangledName.cpp).
    std::string entryPointUsed;
};

} // namespace interpreter