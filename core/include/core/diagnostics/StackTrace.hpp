#pragma once

/// @file StackTrace.hpp
/// @brief Fatal-path stack trace printer for assertion failures.
///
/// This is intentionally minimal and lives outside the diagnostic engine:
/// it must be callable from anywhere (including BaseAST.hpp, which cannot
/// depend on DiagnosticEngine), and it must not allocate in ways that could
/// themselves assert.
///
/// @warning Not async-signal-safe. Use only from the normal abort path,
///          never from a SIGSEGV/SIGABRT handler.

namespace lucid::diag {

/// @brief Print a stack trace to stderr.
///
/// @param skipFrames Number of innermost frames to skip. The caller should
///        pass the number of frames between it and the code the user cares
///        about. For an assert macro, 2 is usually right (skip the macro's
///        helper and the printer itself).
void printStackTrace(int skipFrames = 2);

} // namespace lucid::diag