/// @file MemoryRuntime.cpp
/// @brief The runtime's one allocator, and its `extern "C"` entry points.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// Implements `heapAlloc` / `heapFree` (declared in RuntimeInternal.hpp) and
/// the three memory rows of `functions.def`: `__lucid_alloc`, `__lucid_free`,
/// `__lucid_leak_report`.
///
/// ─── Memory Model ─────────────────────────────────────────────────────────────
/// Lucid uses a simple allocator with explicit free. Every live allocation is
/// tracked in a registry, which detects double-free and invalid-free and
/// backs the leak report. String buffers, arena bases and closure
/// environments are allocated here too (functions.def, rule 5), so the leak
/// report sees everything Lucid code can own.
///
/// ─── Signature Contract ───────────────────────────────────────────────────────
/// This file includes `runtime-abi/lucid_runtime.h`, so each definition below
/// is checked against its row in `functions.def` at compile time.

#include "RuntimeInternal.hpp"
#include "runtime-abi/lucid_runtime.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

using lucid::abi::LucidI64;

// ─── Allocation Registry ──────────────────────────────────────────────────────
// Tracks all live heap allocations: pointer → rounded size.
//
// The registry is heap-allocated and never destroyed. Concurrency worker
// threads and `__lucid_leak_report` may run during static destruction; a
// function-local static would already be gone by then.

namespace {

struct Registry {
    std::mutex                              mutex;
    std::unordered_map<void*, std::size_t>  live;
    std::atomic<std::uint64_t>              totalAllocated{0};
    std::atomic<std::uint64_t>              totalFreed{0};
};

Registry& registry() {
    static Registry* instance = new Registry;   // intentionally leaked
    return *instance;
}

} // anonymous namespace

namespace lucid::runtime {

void* heapAlloc(std::size_t size) noexcept {
    if (size == 0 || size > SIZE_MAX - 7) {
        return nullptr;
    }

    // Round up to 8 bytes for consistency.
    const std::size_t rounded = (size + 7) & ~static_cast<std::size_t>(7);

    void* ptr = std::malloc(rounded);
    if (!ptr) {
        return nullptr;
    }
    std::memset(ptr, 0, rounded);

    Registry& reg = registry();
    try {
        std::lock_guard<std::mutex> lock(reg.mutex);
        reg.live[ptr] = rounded;
    } catch (...) {
        // The registry could not grow. Treat it as an allocation failure
        // rather than handing out a block we cannot track.
        std::free(ptr);
        return nullptr;
    }
    reg.totalAllocated += rounded;
    return ptr;
}

void heapFree(void* ptr) {
    if (!ptr) {
        return;  // Freeing null is a no-op
    }

    Registry& reg = registry();
    std::size_t size = 0;
    bool known = false;
    {
        std::lock_guard<std::mutex> lock(reg.mutex);
        auto it = reg.live.find(ptr);
        if (it != reg.live.end()) {
            known = true;
            size = it->second;
            reg.live.erase(it);
        }
    }

    if (!known) {
        // Not a live allocation: a double free, or a pointer that did not
        // come from heapAlloc. Report it outside the lock.
        panic("runtime: double free or invalid pointer");
    }

    reg.totalFreed += size;
    std::free(ptr);
}

std::uint64_t liveAllocations() {
    Registry& reg = registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    return reg.live.size();
}

std::uint64_t liveBytes() {
    Registry& reg = registry();
    return reg.totalAllocated.load() - reg.totalFreed.load();
}

std::uint64_t totalAllocated() { return registry().totalAllocated.load(); }
std::uint64_t totalFreed()     { return registry().totalFreed.load(); }

} // namespace lucid::runtime

extern "C" {

// ─── Memory Management ──────────────────────────────────────────────────────

void* __lucid_alloc(LucidI64 size) {
    if (size <= 0) {
        return nullptr;
    }

    void* ptr = lucid::runtime::heapAlloc(static_cast<std::size_t>(size));
    if (!ptr) {
        lucid::runtime::panic("runtime: memory allocation failed");
    }
    return ptr;
}

void __lucid_free(void* ptr) {
    lucid::runtime::heapFree(ptr);
}

void __lucid_leak_report() {
    std::fprintf(stderr, "leak report: %llu live allocation(s), %llu byte(s)\n",
                 static_cast<unsigned long long>(lucid::runtime::liveAllocations()),
                 static_cast<unsigned long long>(lucid::runtime::liveBytes()));
}

} // extern "C"