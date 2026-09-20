/// @file runtime/ArenaRuntime.cpp
/// @brief Implementation of arena runtime functions.
///
/// ─── Layout ───────────────────────────────────────────────────────────────────
/// The arena structs are `lucid::abi::LucidArena` and
/// `lucid::abi::LucidArenaDescriptor` from `runtime-abi/lucid_abi.h`. This
/// file does not define its own copies; a private copy is exactly the drift
/// the ABI header exists to prevent.
///
/// ─── Signature Contract ───────────────────────────────────────────────────────
/// This file includes `runtime-abi/lucid_runtime.h`, so each definition below
/// is checked against its row in `functions.def`. Sizes and alignments are
/// `I64`, which is signed: a negative value is a caller error and is rejected
/// like zero, rather than being read as an enormous unsigned size.
///
/// ─── Memory ───────────────────────────────────────────────────────────────────
/// The backing region comes from `lucid::runtime::heapAlloc`, so it shows up
/// in the leak report until `__lucid_arena_free` releases it.

#include "RuntimeInternal.hpp"
#include "runtime-abi/lucid_runtime.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

using lucid::abi::LucidArena;
using lucid::abi::LucidArenaDescriptor;
using lucid::abi::LucidBool;
using lucid::abi::LucidI64;

// ─── Internal Helpers ─────────────────────────────────────────────────────

namespace {

constexpr LucidI64 kDefaultAlignment = 16;

bool isPowerOfTwo(LucidI64 value) {
    return value > 0 && (value & (value - 1)) == 0;
}

/// An arena is valid if its fields are self-consistent. `base` may be null
/// for an empty arena (`Arena::empty()`), but only if `size` is 0.
bool isValidArena(const LucidArena* arena) {
    if (!arena) return false;
    if (arena->size < 0 || arena->cursor < 0) return false;
    if (arena->base == nullptr && arena->size > 0) return false;
    if (arena->cursor > arena->size) return false;
    return true;
}

} // anonymous namespace

extern "C" {

// ─── Arena Create ──────────────────────────────────────────────────────────

void __lucid_arena_create(LucidArenaDescriptor* out, LucidI64 size) {
    if (!out) {
        return;
    }

    out->base = nullptr;
    out->size = 0;

    // Arena::create(0) is not allowed, and a negative size is nonsense.
    if (size <= 0) {
        return;
    }

    // heapAlloc returns zeroed, registered memory, or null on failure.
    void* memory = lucid::runtime::heapAlloc(static_cast<std::size_t>(size));
    if (!memory) {
        return;
    }

    out->base = memory;
    out->size = size;
}

// ─── Arena Free ───────────────────────────────────────────────────────────

void __lucid_arena_free(LucidArena* arena) {
    if (!arena) {
        return;
    }
    if (arena->base) {
        lucid::runtime::heapFree(arena->base);
        arena->base = nullptr;
    }
    arena->size = 0;
    arena->cursor = 0;
}

// ─── Arena Alloc ───────────────────────────────────────────────────────────

void* __lucid_arena_alloc(LucidArena* arena, LucidI64 size, LucidI64 alignment) {
    if (!isValidArena(arena) || size <= 0) {
        return nullptr;
    }

    if (alignment == 0) {
        alignment = kDefaultAlignment;
    }
    if (!isPowerOfTwo(alignment)) {
        return nullptr;
    }

    // Align the ADDRESS, not just the offset: for an alignment larger than
    // the base's own alignment the two differ. All arithmetic is unsigned
    // and checked, so an enormous `size` cannot wrap around and pass the
    // capacity test.
    const std::uint64_t mask = static_cast<std::uint64_t>(alignment) - 1;
    const std::uint64_t base = reinterpret_cast<std::uintptr_t>(arena->base);
    const std::uint64_t address = base + static_cast<std::uint64_t>(arena->cursor);

    if (address > UINT64_MAX - mask) {
        return nullptr;
    }
    const std::uint64_t alignedAddress = (address + mask) & ~mask;
    const std::uint64_t start = alignedAddress - base;   // offset of the block

    const std::uint64_t capacity = static_cast<std::uint64_t>(arena->size);
    if (start > capacity || static_cast<std::uint64_t>(size) > capacity - start) {
        return nullptr;   // out of capacity
    }

    void* result = static_cast<std::uint8_t*>(arena->base) + start;
    arena->cursor = static_cast<LucidI64>(start + static_cast<std::uint64_t>(size));

    std::memset(result, 0, static_cast<std::size_t>(size));
    return result;
}

// ─── Arena Reset ──────────────────────────────────────────────────────────

void __lucid_arena_reset(LucidArena* arena) {
    if (arena) {
        arena->cursor = 0;
    }
}

// ─── Arena Capacity ───────────────────────────────────────────────────────

LucidI64 __lucid_arena_capacity(LucidArena* arena) {
    if (!isValidArena(arena)) {
        return 0;
    }
    return arena->size;
}

// ─── Arena Remaining ──────────────────────────────────────────────────────

LucidI64 __lucid_arena_remaining(LucidArena* arena) {
    if (!isValidArena(arena)) {
        return 0;
    }
    return arena->size - arena->cursor;
}

// ─── Arena Is Empty ───────────────────────────────────────────────────────

LucidBool __lucid_arena_is_empty(LucidArena* arena) {
    if (!isValidArena(arena)) {
        return 1;
    }
    return arena->cursor == 0 ? 1 : 0;
}

// ─── Arena Space ──────────────────────────────────────────────────────────

LucidI64 __lucid_arena_space(LucidArena* arena, LucidI64 elemSize) {
    if (!isValidArena(arena) || elemSize <= 0) {
        return 0;
    }
    return (arena->size - arena->cursor) / elemSize;
}

// ─── Arena Can Fit ────────────────────────────────────────────────────────

LucidBool __lucid_arena_can_fit(LucidArena* arena, LucidI64 elemSize, LucidI64 count) {
    if (!isValidArena(arena) || elemSize <= 0 || count < 0) {
        return 0;
    }
    if (count == 0) {
        return 1;
    }
    // `count * elemSize <= remaining`, written as a division so the product
    // cannot overflow.
    const LucidI64 remaining = arena->size - arena->cursor;
    return count <= remaining / elemSize ? 1 : 0;
}

} // extern "C"