/// @file runtime/arena/ArenaRuntime.cpp
/// @brief Implementation of arena runtime functions.

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

// ─── Arena struct (private to runtime) ────────────────────────────────────
// This matches the LLVM type defined in CodeGenContext::getArenaType()
struct Arena {
    void* base;      // Pointer to allocated memory
    uint64_t size;   // Total capacity in bytes
    uint64_t cursor; // Current allocation position
};

// ─── ArenaDescriptor struct (FFI-visible) ─────────────────────────────────
// This matches the LLVM type defined in CodeGenContext::getArenaDescriptorType()
struct ArenaDescriptor {
    void* base;      // Pointer to allocated memory
    uint64_t size;   // Total capacity in bytes
};

// ─── Internal Helpers ─────────────────────────────────────────────────────

static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
    uint64_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

static constexpr uint64_t DEFAULT_ALIGNMENT = 16;

static inline bool is_power_of_two(uint64_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

static inline bool is_valid_arena(const Arena* arena_ptr) {
    if (!arena_ptr) return false;
    // base can be null for empty arena (Arena::empty())
    if (arena_ptr->base == nullptr && arena_ptr->size > 0) return false;
    if (arena_ptr->cursor > arena_ptr->size) return false;
    return true;
}

extern "C" {

// ─── Arena Create ──────────────────────────────────────────────────────────

ArenaDescriptor __lucid_arena_create(uint64_t size) {
    ArenaDescriptor desc = {nullptr, 0};
    
    // ─── Reject size 0 ──────────────────────────────────────────────────────
    if (size == 0) {
        return desc;  // Arena::create(0) is not allowed
    }
    
    // ─── Allocate memory ──────────────────────────────────────────────────
    uint64_t alignedSize = (size + 4095) & ~4095;
    void* memory = std::malloc(alignedSize);
    if (!memory) {
        return desc;  // Allocation failed
    }
    
    std::memset(memory, 0, alignedSize);
    
    desc.base = memory;
    desc.size = alignedSize;
    return desc;
}

// ─── Arena Alloc ───────────────────────────────────────────────────────────

void* __lucid_arena_alloc(Arena* arena_ptr, uint64_t size, uint64_t alignment) {
    if (!is_valid_arena(arena_ptr)) {
        return nullptr;
    }
    
    if (size == 0) {
        return nullptr;
    }
    
    if (alignment == 0) {
        alignment = DEFAULT_ALIGNMENT;
    }
    if (!is_power_of_two(alignment)) {
        return nullptr;
    }
    
    uint64_t current = arena_ptr->cursor;
    uint64_t aligned = align_up(current, alignment);
    uint64_t new_cursor = aligned + size;
    
    if (new_cursor > arena_ptr->size) {
        return nullptr;
    }
    
    uint8_t* base = static_cast<uint8_t*>(arena_ptr->base);
    void* result = base + aligned;
    arena_ptr->cursor = new_cursor;
    
    std::memset(result, 0, size);
    
    return result;
}

// ─── Arena Reset ──────────────────────────────────────────────────────────

void __lucid_arena_reset(Arena* arena_ptr) {
    if (arena_ptr) {
        arena_ptr->cursor = 0;
        if (arena_ptr->base && arena_ptr->size > 0) {
            std::memset(arena_ptr->base, 0, arena_ptr->size);
        }
    }
}

// ─── Arena Capacity ───────────────────────────────────────────────────────

uint64_t __lucid_arena_capacity(const Arena* arena_ptr) {
    if (!is_valid_arena(arena_ptr)) {
        return 0;
    }
    return arena_ptr->size;
}

// ─── Arena Remaining ──────────────────────────────────────────────────────

uint64_t __lucid_arena_remaining(const Arena* arena_ptr) {
    if (!is_valid_arena(arena_ptr)) {
        return 0;
    }
    return arena_ptr->size - arena_ptr->cursor;
}

// ─── Arena Is Empty ───────────────────────────────────────────────────────

bool __lucid_arena_is_empty(const Arena* arena_ptr) {
    if (!is_valid_arena(arena_ptr)) {
        return true;
    }
    return arena_ptr->cursor == 0;
}

// ─── Arena Space ──────────────────────────────────────────────────────────

uint64_t __lucid_arena_space(const Arena* arena_ptr, uint64_t elem_size) {
    if (!is_valid_arena(arena_ptr) || elem_size == 0) {
        return 0;
    }
    
    uint64_t remaining = arena_ptr->size - arena_ptr->cursor;
    return remaining / elem_size;
}

// ─── Arena Can Fit ────────────────────────────────────────────────────────

bool __lucid_arena_can_fit(const Arena* arena_ptr, uint64_t elem_size, uint64_t count) {
    if (!is_valid_arena(arena_ptr) || elem_size == 0) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    
    uint64_t needed = elem_size * count;
    uint64_t remaining = arena_ptr->size - arena_ptr->cursor;
    return needed <= remaining;
}

} // extern "C"