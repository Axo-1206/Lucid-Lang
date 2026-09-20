/// @file ClosureRuntime.cpp
/// @brief Extern "C" entry points for the Lucid closure runtime.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" functions that are called by
/// JIT-compiled and AOT-compiled Lucid code: the closure rows of
/// `runtime-abi/functions.def`.
///
/// ─── Signature Contract ───────────────────────────────────────────────────────
/// This file includes `runtime-abi/lucid_runtime.h`, so each definition below
/// is checked against its row at compile time. That is why `drop` is a
/// `void*` here and not a function pointer: the row's tag is `Ptr`. It is
/// converted to `ClosureEnvHeader::DropFn` inside `__lucid_alloc_env`.
///
/// ─── Important ──────────────────────────────────────────────────────────────
/// These functions MUST be reachable by JIT-compiled code. In the final
/// binary, they are either:
///   - Inside lucid.exe (for JIT mode)
///   - Linked into game.exe (for AOT mode)
///
/// ─── ABI Stability ──────────────────────────────────────────────────────────
/// These functions form a stable ABI between the compiler and the runtime.
/// Changing their signatures means changing their rows in functions.def;
/// the compiler and the runtime then follow from the table.

#include "ClosureEnvironment.hpp"
#include "runtime-abi/lucid_runtime.h"

#include <cstdint>
#include <cstddef>

using lucid::abi::LucidI64;

extern "C" {

// ─── Closure Environment Management ──────────────────────────────────────────

/// @brief Allocate a closure environment.
/// @param size Size of the data portion in bytes. Must be positive: a
///        non-capturing closure has a null env and never calls this.
/// @param drop Optional (may be null). A `void (*)(void* data)`. Called with
///        a pointer to the data portion when the last reference is released,
///        before the memory is freed. The compiler emits one per closure that
///        captures a `cls` value by value, to release the captured
///        environments.
/// @return Pointer to the environment (header + data), or nullptr on failure.
void* __lucid_alloc_env(LucidI64 size, void* drop) {
    if (size <= 0) {
        return nullptr;
    }

    return ClosureEnvHeader::allocate(
        static_cast<std::size_t>(size),
        reinterpret_cast<ClosureEnvHeader::DropFn>(drop));
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