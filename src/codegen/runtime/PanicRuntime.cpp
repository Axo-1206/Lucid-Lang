/// @file PanicRuntime.cpp
/// @brief Implementation of panic runtime functions.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" entry point for runtime panics.
/// When a panic occurs, this function prints the message and aborts the program.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {

/// @brief Panic with a message.
/// @param message Null-terminated string containing the error message.
void __lucid_panic(char* message) {
    if (message) {
        // In a real implementation, this would print to stderr and abort
        // In the interpreter, it might throw an exception or return to the REPL
        fprintf(stderr, "\npanic: %s\n", message);
    } else {
        fprintf(stderr, "\npanic: unknown error\n");
    }
    // For now, abort the program
    // In a full implementation, this would unwind or return to the interpreter
    std::abort();
}

} // extern "C"