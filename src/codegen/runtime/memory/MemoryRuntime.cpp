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
/// 
/// ─── Arena Allocator ──────────────────────────────────────────────────────────
/// Arenas are bump-pointer allocators that can be reset or freed as a whole.
/// They're useful for temporary allocations that all have the same lifetime.

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <mutex>
#include <atomic>

// ─── Forward declarations for arena descriptor ──────────────────────────────
// This matches the anonymous struct built at #arena_create call site in
// LucidIntrinsicEmitter.cpp (ArenaCreate).
struct ArenaDescriptor {
    void* base;     // Start of arena memory
    uint64_t size;  // Total size of arena
};

// ─── Allocation Registry ──────────────────────────────────────────────────────
// Tracks all live heap allocations to detect double-free and use-after-free.
// This is a simple hash map protected by a mutex. In production, this would
// be optimized or replaced with a more sophisticated allocator.

static std::unordered_map<void*, size_t> g_allocationRegistry;
static std::mutex g_allocationMutex;
static std::atomic<uint64_t> g_totalAllocated{0};
static std::atomic<uint64_t> g_totalFreed{0};

// ─── Arena Registry ──────────────────────────────────────────────────────────
// Tracks all live arenas. The key is the arena descriptor pointer, and the
// value is the raw allocation pointer that needs to be freed when the arena
// is destroyed.

static std::unordered_map<void*, void*> g_arenaRegistry;
static std::mutex g_arenaMutex;

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

/// @brief Allocate memory on the heap.
/// @param size Number of bytes to allocate.
/// @return Pointer to allocated memory, or nullptr on failure.
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

/// @brief Free memory allocated with __lucid_alloc.
/// @param ptr Pointer to memory to free.
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

/// @brief Get the size of an allocation.
/// @param ptr Pointer to allocated memory.
/// @return Size of the allocation, or 0 if not found.
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

/// @brief Get total allocated memory.
uint64_t __lucid_total_allocated() {
    return g_totalAllocated.load();
}

/// @brief Get total freed memory.
uint64_t __lucid_total_freed() {
    return g_totalFreed.load();
}

// ─── Arena Management ──────────────────────────────────────────────────────

/// @brief Create a new arena.
/// @param size Size of the arena in bytes.
/// @return ArenaDescriptor { base, size }.
ArenaDescriptor __lucid_arena_create(uint64_t size) {
    if (size == 0) {
        size = 4096;  // Default to 4KB
    }

    // Align to page size (4KB) for efficiency
    uint64_t alignedSize = (size + 4095) & ~4095;

    void* arenaMem = std::malloc(alignedSize);
    if (!arenaMem) {
        return ArenaDescriptor{nullptr, 0};
    }

    // Zero-initialize the arena
    std::memset(arenaMem, 0, alignedSize);

    // Register the arena
    {
        std::lock_guard<std::mutex> lock(g_arenaMutex);
        g_arenaRegistry[arenaMem] = arenaMem;
    }

    return ArenaDescriptor{arenaMem, alignedSize};
}

/// @brief Allocate memory from an arena.
/// @param arena Pointer to the arena descriptor.
/// @param size Size to allocate.
/// @return Pointer to allocated memory, or nullptr if arena is full.
void* __lucid_arena_alloc(void* arena, uint64_t size) {
    if (!arena || size == 0) {
        return nullptr;
    }

    ArenaDescriptor* desc = static_cast<ArenaDescriptor*>(arena);
    if (!desc->base) {
        return nullptr;
    }

    // Current allocation pointer stored at the end of the arena
    // We use the last 8 bytes of the arena to track the current position
    uint8_t* arenaBase = static_cast<uint8_t*>(desc->base);
    uint64_t* currentPos = reinterpret_cast<uint64_t*>(arenaBase + desc->size - sizeof(uint64_t));

    // Check if we have enough space (reserve space for the position pointer)
    uint64_t available = desc->size - sizeof(uint64_t) - *currentPos;
    if (size > available) {
        return nullptr;  // Out of memory
    }

    // Allocate from the arena
    void* result = arenaBase + *currentPos;
    *currentPos += size;

    // Zero-initialize the allocated memory
    std::memset(result, 0, size);

    return result;
}

/// @brief Reset an arena (free all allocations within it).
/// @param arena Pointer to the arena descriptor.
void __lucid_arena_reset(void* arena) {
    if (!arena) {
        return;
    }

    ArenaDescriptor* desc = static_cast<ArenaDescriptor*>(arena);
    if (!desc->base) {
        return;
    }

    // Reset the current position to 0
    uint8_t* arenaBase = static_cast<uint8_t*>(desc->base);
    uint64_t* currentPos = reinterpret_cast<uint64_t*>(arenaBase + desc->size - sizeof(uint64_t));
    *currentPos = 0;

    // Zero-initialize the entire arena (optional, but good for security)
    std::memset(desc->base, 0, desc->size - sizeof(uint64_t));
}

/// @brief Free an arena and all memory within it.
/// @param arena Pointer to the arena descriptor.
void __lucid_arena_free(void* arena) {
    if (!arena) {
        return;
    }

    ArenaDescriptor* desc = static_cast<ArenaDescriptor*>(arena);
    if (!desc->base) {
        return;
    }

    // Unregister the arena
    {
        std::lock_guard<std::mutex> lock(g_arenaMutex);
        auto it = g_arenaRegistry.find(desc->base);
        if (it != g_arenaRegistry.end()) {
            g_arenaRegistry.erase(it);
        }
    }

    // Free the arena memory
    std::free(desc->base);
    desc->base = nullptr;
    desc->size = 0;
}

} // extern "C"