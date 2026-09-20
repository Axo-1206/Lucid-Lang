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

namespace lucid::runtime {

void panic(const char* message) {
    if (message) {
        std::fprintf(stderr, "\npanic: %s\n", message);
    } else {
        std::fprintf(stderr, "\npanic: unknown error\n");
    }
    std::fflush(stderr);
    // For now, abort the program
    // In a full implementation, this would unwind or return to the interpreter
    std::abort();
}

} // namespace lucid::runtime

extern "C" {

/// @brief Panic with a message.
/// @param message NUL-terminated `const char*`, passed as `void*` because the
///                row's tag is `Ptr`. Format: "file:line:column: description"
///
/// ─── Example ──────────────────────────────────────────────────────────────────
///   __lucid_panic("main.luc:42:10: division by zero");
///
///   Output:
///   panic: main.luc:42:10: division by zero
void __lucid_panic(void* message) {
    lucid::runtime::panic(static_cast<const char*>(message));
}

} // extern "C"