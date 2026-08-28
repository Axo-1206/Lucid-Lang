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

/// @brief Check if a value is a closure environment.
/// @param value Pointer to check.
/// @return 1 if the value is a closure environment, 0 otherwise.
///
/// ─── Implementation ──────────────────────────────────────────────────────────
/// This function checks if the pointer is a valid closure environment using:
///   1. Null check
///   2. Alignment check (must be 8-byte aligned)
///   3. Magic number check (if available)
///
/// For backwards compatibility, we fall back to alignment check if the
/// magic number is not set (older environments).
///
/// ─── Used By ─────────────────────────────────────────────────────────────────
/// Called from CodeGenExpr.cpp in emitCallableCall() when a function-typed
/// value might be either a plain function or a closure. This is the runtime
/// check for the conservative isClosureValue = true case.
int __lucid_is_closure(void* value) {
    if (!value) {
        return 0;
    }

    // ─── 1. Alignment check ──────────────────────────────────────────────────
    // Closure environments are 8-byte aligned. Function pointers are not.
    if (reinterpret_cast<uintptr_t>(value) % alignof(ClosureEnvHeader) != 0) {
        return 0;
    }

    // ─── 2. Try to read the magic number from the padding field ─────────────
    // If the magic number is set, it's definitely a closure environment.
    ClosureEnvHeader* header = static_cast<ClosureEnvHeader*>(value);
    
    // Check if the padding field contains the magic number.
    // Note: This requires the allocate method to set _padding = CLOSURE_MAGIC.
    // If the magic number is not set (older environments), we fall back to
    // the alignment check which is less reliable but still works for most cases.
    if (header->_padding == CLOSURE_MAGIC) {
        return 1;
    }

    // ─── 3. Fallback: Basic validity check ─────────────────────────────────
    // Check if the refcount is reasonable (not zero, not absurdly large)
    // This is a heuristic and may have false positives/negatives.
    uint32_t refcount = ClosureEnvHeader::getRefcount(header);
    if (refcount > 0 && refcount < 1000000) {
        // Could be a valid environment with a reasonable refcount.
        // However, we can't be 100% sure without the magic number.
        // Return 1 as a conservative estimate.
        return 1;
    }

    // ─── 4. Use the existing isValid check ──────────────────────────────────
    // This checks alignment and size consistency.
    return ClosureEnvHeader::isValid(value) ? 1 : 0;
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

/// @brief Get the current reference count of a closure environment.
/// @param env Pointer to the environment.
/// @return The current reference count.
uint32_t __lucid_env_refcount(void* env) {
    auto* header = static_cast<ClosureEnvHeader*>(env);
    return ClosureEnvHeader::getRefcount(header);
}

} // extern "C"