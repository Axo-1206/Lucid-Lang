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
void* __lucid_alloc_env(uint64_t size) {
    // Validate size
    if (size == 0) {
        return nullptr;
    }

    if (size > UINT32_MAX) {
        return nullptr;
    }

    return ClosureEnvHeader::allocate(static_cast<uint32_t>(size));
}

/// @brief Retain a closure environment (increment reference count).
/// @param env Pointer to the environment (from __lucid_alloc_env).
void __lucid_retain_env(void* env) {
    auto* header = static_cast<ClosureEnvHeader*>(env);
    ClosureEnvHeader::retain(header);
}

/// @brief Release a closure environment (decrement reference count).
/// @param env Pointer to the environment (from __lucid_alloc_env).
void __lucid_release_env(void* env) {
    auto* header = static_cast<ClosureEnvHeader*>(env);
    ClosureEnvHeader::release(header);
}

} // extern "C"