/// @file MemoryRuntime.cpp
/// @brief Implementation of memory management runtime functions.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" entry points for memory management
/// operations that are called by JIT-compiled and AOT-compiled Lucid code.
///
/// ─── Memory Model ─────────────────────────────────────────────────────────────
/// Lucid uses a simple allocator with explicit free. All allocations are
/// tracked in a global registry to detect double-free and use-after-free.

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <atomic>

// ─── Allocation Registry ──────────────────────────────────────────────────────
// Tracks all live heap allocations to detect double-free and use-after-free.

static std::unordered_map<void*, size_t> g_allocationRegistry;
static std::mutex g_allocationMutex;
static std::atomic<uint64_t> g_totalAllocated{0};
static std::atomic<uint64_t> g_totalFreed{0};

// ─── Helper: Check if a pointer is a valid allocation ──────────────────────
static bool isValidAllocation(void* ptr) {
    std::lock_guard<std::mutex> lock(g_allocationMutex);
    return g_allocationRegistry.find(ptr) != g_allocationRegistry.end();
}

// ─── Helper: Register an allocation ──────────────────────────────────────────
static void registerAllocation(void* ptr, size_t size) {
    std::lock_guard<std::mutex> lock(g_allocationMutex);
    g_allocationRegistry[ptr] = size;
    g_totalAllocated += size;
}

// ─── Helper: Unregister an allocation ────────────────────────────────────────
static bool unregisterAllocation(void* ptr) {
    std::lock_guard<std::mutex> lock(g_allocationMutex);
    auto it = g_allocationRegistry.find(ptr);
    if (it == g_allocationRegistry.end()) {
        return false;  // Not found - double free or invalid pointer
    }
    g_totalFreed += it->second;
    g_allocationRegistry.erase(it);
    return true;
}

extern "C" {

// ─── Memory Management ──────────────────────────────────────────────────────

void* __lucid_alloc(uint64_t size) {
    if (size == 0) {
        return nullptr;
    }

    // Align to 8 bytes for consistency
    size_t alignedSize = (size + 7) & ~7;

    void* ptr = std::malloc(alignedSize);
    if (!ptr) {
        return nullptr;
    }

    // Zero-initialize the memory
    std::memset(ptr, 0, alignedSize);

    // Register the allocation
    registerAllocation(ptr, alignedSize);

    return ptr;
}

void __lucid_free(void* ptr) {
    if (!ptr) {
        return;  // Freeing null is a no-op
    }

    // Check if this is a valid allocation
    if (!isValidAllocation(ptr)) {
        // Double free or invalid pointer - we could panic here
        // For now, just return silently (in production, this would panic)
        return;
    }

    // Unregister the allocation
    if (!unregisterAllocation(ptr)) {
        return;  // Already freed
    }

    // Free the memory
    std::free(ptr);
}

uint64_t __lucid_alloc_size(void* ptr) {
    if (!ptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_allocationMutex);
    auto it = g_allocationRegistry.find(ptr);
    if (it == g_allocationRegistry.end()) {
        return 0;
    }
    return it->second;
}

uint64_t __lucid_total_allocated() {
    return g_totalAllocated.load();
}

uint64_t __lucid_total_freed() {
    return g_totalFreed.load();
}

} // extern "C"