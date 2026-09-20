/// @file runtime/RuntimeInternal.hpp
/// @brief C++-only helpers shared by the runtime's translation units.
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The runtime's internal API: the one allocator and the panic path. The
/// `extern "C"` entry points in `MemoryRuntime.cpp` and `PanicRuntime.cpp`
/// are thin wrappers over these, and the other runtime files (strings,
/// arenas, closures, concurrency) call these directly instead of going
/// through the C ABI.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT part of the ABI. Nothing here appears in `functions.def`,
/// generated code cannot call it, and it may change freely. That is the
/// point of keeping it out of the `__lucid_` namespace: a `__lucid_*` symbol
/// with no row in the table is a bug (rule 7 in `functions.def`).
///
/// ─── The One Allocator ────────────────────────────────────────────────────
/// Every heap block the runtime hands out is allocated by `heapAlloc` and
/// released by `heapFree`. `heapAlloc` records the block in a registry, which
/// is what lets `__lucid_leak_report` count live blocks and lets `heapFree`
/// reject a pointer that is not a live allocation. A block allocated with
/// plain `malloc` and released with `__lucid_free` would be reported as an
/// invalid free — so string buffers, arena bases and closure environments
/// must not bypass it.

#pragma once

#include <cstddef>
#include <cstdint>

namespace lucid::runtime {

/// @brief Allocate `size` bytes of zeroed, registered memory.
/// @return The block, or nullptr if `size` is 0 or the system is out of
///         memory. Never panics: each caller has its own documented failure
///         path (`__lucid_alloc` panics; string and arena constructors leave
///         their out-slot empty; `__lucid_alloc_env` returns null).
/// @note   The block is aligned as `malloc` aligns (16 bytes on the 64-bit
///         targets Lucid supports) and its size is rounded up to 8.
void* heapAlloc(std::size_t size) noexcept;

/// @brief Release a block returned by `heapAlloc`. Null-safe.
///
/// Panics if `ptr` is not a live allocation (double free, or a pointer that
/// never came from `heapAlloc`).
void heapFree(void* ptr);

/// @name Registry statistics (for `__lucid_leak_report` and unit tests)
/// @{
std::uint64_t liveAllocations();
std::uint64_t liveBytes();
std::uint64_t totalAllocated();
std::uint64_t totalFreed();
/// @}

/// @brief Print `message` to stderr as `panic: <message>` and abort.
///
/// `message` may be null. This is the C++ face of `__lucid_panic`; runtime
/// code calls it instead of the C entry point so it can pass a
/// `const char*` without casting to the ABI's `void*`.
[[noreturn]] void panic(const char* message);

} // namespace lucid::runtime