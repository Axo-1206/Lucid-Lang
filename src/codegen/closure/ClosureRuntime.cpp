/// @file ClosureRuntime.cpp
/// @brief Extern "C" entry points for the Lucid closure runtime.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" functions that are called by
/// JIT-compiled and AOT-compiled Lucid code. These functions are declared
/// in RuntimeFunctionRegistry.hpp and called via LLVM IR calls.
///
/// ─── Important ──────────────────────────────────────────────────────────────
/// These functions MUST be exported from the binary (lucid.exe or game.exe)
/// so that JIT-compiled code can find them. In the final binary, they are
/// either:
///   - Inside lucid.exe (for JIT mode)
///   - Linked into game.exe (for AOT mode)
///
/// ─── ABI Stability ──────────────────────────────────────────────────────────
/// These functions form a stable ABI between the compiler and the runtime.
/// Changing their signatures requires updating both the compiler and the
/// runtime implementation.

#include "ClosureEnvironment.hpp"
#include <cstdint>
#include <cstddef>

extern "C" {

// ─── Closure Environment Management ──────────────────────────────────────────

/// @brief Allocate a closure environment.
/// @param size Size of the data portion in bytes.
/// @return Pointer to the environment (header + data), or nullptr on failure.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenClosure.cpp in lowerClosure() when a closure is created.
/// The environment is allocated with a reference count of 1.
///
/// ─── Memory Layout ──────────────────────────────────────────────────────────
/// The returned pointer points to the ClosureEnvHeader, which is immediately
/// followed by the captured data. The caller stores this pointer in the
/// closure value's environment slot.
void* __lucid_alloc_env(uint64_t size) {
    // Validate size
    if (size == 0) {
        // Zero-sized environment - return a null pointer
        // The closure will handle null env pointer gracefully
        return nullptr;
    }

    // Clamp size to prevent overflow
    if (size > UINT32_MAX) {
        // Environment size is too large - panic
        // Note: This should never happen in practice
        return nullptr;
    }

    return ClosureEnvHeader::allocate(static_cast<uint32_t>(size));
}

/// @brief Check if a value is a closure environment.
/// @param value Pointer to check.
/// @return 1 if the value is a closure environment, 0 otherwise.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenExpr.cpp in emitCallableCall() when a function-typed
/// value might be either a plain function or a closure. This is the runtime
/// check for the conservative isClosureValue = true case.
///
/// ─── Implementation Note ──────────────────────────────────────────────────
/// The check is conservative: it uses alignment checking to determine if the
/// pointer looks like a valid environment header. This is fast but not 100%
/// foolproof. For a stronger check, we could add a magic number to the header.
int __lucid_is_closure(void* value) {
    return ClosureEnvHeader::isValid(value) ? 1 : 0;
}

/// @brief Retain a closure environment (increment reference count).
/// @param env Pointer to the environment (from __lucid_alloc_env).
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenExpr.cpp in lowerAssignExpr() when a closure value is
/// copied (assigned to a variable, passed as an argument, etc.).
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// let f = makeCounter();  // f holds a closure, env refcount = 1
/// let g = f;              // Copy f → __lucid_retain_env(env) → refcount = 2
void __lucid_retain_env(void* env) {
    auto* header = static_cast<ClosureEnvHeader*>(env);
    ClosureEnvHeader::retain(header);
}

/// @brief Release a closure environment (decrement reference count).
/// @param env Pointer to the environment (from __lucid_alloc_env).
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenStmt.cpp in lowerBlockStmt(), lowerReturnStmt(),
/// lowerBreakStmt(), and lowerContinueStmt() when a closure variable goes
/// out of scope.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// let f = makeCounter();  // env refcount = 1
/// // ... use f ...
/// // Block exits → __lucid_release_env(env) → refcount = 0 → free
void __lucid_release_env(void* env) {
    auto* header = static_cast<ClosureEnvHeader*>(env);
    ClosureEnvHeader::release(header);
}

/// @brief Get the current reference count of a closure environment.
/// @param env Pointer to the environment.
/// @return The current reference count.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Debugging only - not called by compiler-generated code.
uint32_t __lucid_env_refcount(void* env) {
    auto* header = static_cast<ClosureEnvHeader*>(env);
    return ClosureEnvHeader::getRefcount(header);
}

} // extern "C"