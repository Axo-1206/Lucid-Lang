/// @file PanicRuntime.cpp
/// @brief Implementation of panic runtime functions.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the panic path. When a panic occurs, the message is
/// printed and the program aborts.
///
///   - `lucid::runtime::panic` is the C++ function the rest of the runtime
///     calls (declared `[[noreturn]]` in RuntimeInternal.hpp).
///   - `__lucid_panic` is the `extern "C"` entry point generated code calls,
///     the `Panic` row of `functions.def`.
///
/// ─── Why Two Functions ────────────────────────────────────────────────────────
/// The row's parameter tag is `Ptr`, which is `void*`, so the C entry point
/// takes `void*` and casts. The C++ function takes `const char*`, so runtime
/// code does not have to cast. The C entry point is not itself declared
/// `[[noreturn]]`: the attribute would have to appear on the first
/// declaration, which is the table-generated prototype, and the table has no
/// way to say it. `Abi::declareOrGet` marks the LLVM declaration `noreturn`
/// instead, which is the property generated code needs.
///
/// ─── Panic Message Format ────────────────────────────────────────────────────
/// The compiler passes messages in the format:
///   "file:line:column: error description"
///
/// Examples:
///   "main.luc:42:10: division by zero"
///   "main.luc:15:5: array index out of bounds"
///   "main.luc:8:3: arena out of capacity"
///
/// The message is already formatted by the compiler's `buildPanicMessage()`
/// function before being passed to __lucid_panic. Messages raised by the
/// runtime itself carry a "runtime: " prefix instead of a source location.
///
/// ─── Future Enhancements ─────────────────────────────────────────────────────
/// In a full implementation, this function would:
///   - Print to stderr with color
///   - Optionally write to a log file
///   - In the interpreter, throw an exception or return to the REPL
///   - In AOT mode, abort the program with the error code

#include "RuntimeInternal.hpp"
#include "runtime-abi/lucid_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#  include <io.h>
#  include <process.h>
#  define WRITE_FD(fd, buf, len) _write(fd, buf, static_cast<unsigned>(len))
#  define STDERR_FD 2
#  define EXIT_NOW() _exit(1)
#else
#  include <unistd.h>
#  define WRITE_FD(fd, buf, len) write(fd, buf, len)
#  define STDERR_FD STDERR_FILENO
#  define EXIT_NOW() _exit(1)
#endif

namespace lucid::runtime {

namespace {

/// Async-signal-safe write of a NUL-terminated string to stderr.
///
/// Uses `write(2)` (or `_write` on Windows), which is on POSIX's
/// async-signal-safe list. Does not allocate, does not take stdio locks,
/// does not use iostream. Safe to call from:
///
///   - A panicking thread, even if another thread holds the stderr lock.
///   - A signal handler (SIGSEGV for stack overflow, SIGABRT, ...).
///   - Multiple threads simultaneously; the kernel serializes writes at
///     the file-descriptor level per-call, so output is at worst
///     interleaved at byte boundaries, never lost or truncated.
///
/// The message is written in two calls (prefix, then message) rather than
/// one, so no buffer is needed. On a POSIX pipe, writes under PIPE_BUF
/// are atomic; interleaving between threads is bounded by that.
void asyncSafeWriteStderr(const char* s) {
    if (!s) return;
    const size_t len = std::strlen(s);
    if (len == 0) return;
    // Best-effort: ignore the return value. If the write fails (stderr
    // closed, disk full), there is nothing useful to do at panic time.
    (void)WRITE_FD(STDERR_FD, s, len);
}

} // anonymous namespace

void panic(const char* message) {
    asyncSafeWriteStderr("\npanic: ");
    asyncSafeWriteStderr(message ? message : "unknown error");
    asyncSafeWriteStderr("\n");

    // Terminate immediately. `_exit` does not run atexit handlers,
    // flush stdio, or invoke other finalizers — which is correct for a
    // panic: the program is in an unknown state and should not attempt
    // cleanup. See PanicRuntime.hpp's file header for the reasoning.
    EXIT_NOW();
}

} // namespace lucid::runtime