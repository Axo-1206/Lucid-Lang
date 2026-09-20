/// @file PanicRuntime.cpp
/// @brief Implementation of panic runtime functions.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" entry point for runtime panics.
/// When a panic occurs, this function prints the message and aborts the program.
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
/// function before being passed to __lucid_panic.
///
/// ─── Future Enhancements ─────────────────────────────────────────────────────
/// In a full implementation, this function would:
///   - Print to stderr with color
///   - Optionally write to a log file
///   - In the interpreter, throw an exception or return to the REPL
///   - In AOT mode, abort the program with the error code

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {

/// @brief Panic with a message.
/// @param message Null-terminated string containing the error message.
///                Format: "file:line:column: error description"
///
/// ─── Example ──────────────────────────────────────────────────────────────────
///   __lucid_panic("main.luc:42:10: division by zero");
///
///   Output:
///   panic: main.luc:42:10: division by zero
void __lucid_panic(char* message) {
    if (message) {
        fprintf(stderr, "\npanic: %s\n", message);
    } else {
        fprintf(stderr, "\npanic: unknown error\n");
    }
    // For now, abort the program
    // In a full implementation, this would unwind or return to the interpreter
    std::abort();
}

} // extern "C"