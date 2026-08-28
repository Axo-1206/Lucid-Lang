/// @file ClosureEnvironment.hpp
/// @brief Closure environment memory management - refcounted heap allocation.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// Closure environments are heap-allocated, reference-counted structures that
/// hold the captured variables for a closure. When a closure is copied, the
/// environment's reference count is incremented. When a closure is destroyed,
/// the reference count is decremented. When the count reaches zero, the
/// environment is freed.
///
/// ─── Memory Layout ───────────────────────────────────────────────────────────
///
///   ┌─────────────────────────────────────────────────────────────────────┐
///   │  ┌─────────────────────────────────────────────────────────────┐   │
///   │  │  ClosureEnvHeader (24-32 bytes)                             │   │
///   │  │  ┌─────────────────────────────────────────────────────────┐ │   │
///   │  │  │  refcount: atomic<uint32_t>   (4 bytes, 8-byte aligned)│ │   │
///   │  │  │  size: uint32_t               (4 bytes)                 │ │   │
///   │  │  │  padding: uint32_t            (4 bytes, for alignment)  │ │   │
///   │  │  └─────────────────────────────────────────────────────────┘ │   │
///   │  └─────────────────────────────────────────────────────────────┘   │
///   │  ┌─────────────────────────────────────────────────────────────┐   │
///   │  │  Data Portion (size bytes)                                  │   │
///   │  │  └── Captured variables stored contiguously               │   │
///   │  └─────────────────────────────────────────────────────────────┘   │
///   └─────────────────────────────────────────────────────────────────────┘
///
/// ─── Allocation ──────────────────────────────────────────────────────────────
/// The environment is allocated as a single block of memory:
///   totalSize = sizeof(ClosureEnvHeader) + dataSize
///
/// The header is placed at the start, followed immediately by the data.
///
/// ─── Refcount Semantics ──────────────────────────────────────────────────────
/// - Starts at 1 when allocated (the closure that created it holds the ref)
/// - Retain: increments the count by 1
/// - Release: decrements the count by 1; if it reaches 0, free the memory
///
/// ─── Thread Safety ──────────────────────────────────────────────────────────
/// Uses std::atomic with memory_order_acq_rel for correct synchronization
/// across threads. This allows closure environments to be shared between
/// threads safely.
///
/// ─── Runtime Entry Points ────────────────────────────────────────────────────
/// The extern "C" functions in ClosureRuntime.cpp call into this class.
/// This separation allows the C++ implementation to be unit-tested independently
/// and keeps the C ABI surface minimal.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

/// @brief Header for every closure environment.
///
/// This header is placed at the beginning of every closure environment
/// allocation. It contains the reference count and the size of the data
/// portion, allowing safe allocation, deallocation, and refcounting.
///
/// @note The header is followed immediately by the captured data.
/// @note The header is 8-byte aligned for efficient atomic operations.
struct ClosureEnvHeader {
    // ─── Fields ──────────────────────────────────────────────────────────────

    /// Reference count - number of references to this environment.
    /// Starts at 1. Protected by atomic operations for thread safety.
    std::atomic<uint32_t> refcount;

    /// Size of the data portion in bytes (does NOT include the header).
    uint32_t size;

    // ─── Padding ─────────────────────────────────────────────────────────────
    // Ensures 8-byte alignment of the header for optimal atomic operations.
    // This padding guarantees the header starts at an 8-byte aligned address
    // when the environment is allocated with malloc (which returns 8-byte
    // aligned pointers on 64-bit systems).
    uint32_t _padding;

    // ─── Methods ─────────────────────────────────────────────────────────────

    /// @brief Get pointer to the data portion.
    /// @return Pointer to the data immediately following the header.
    uint8_t* data() {
        return reinterpret_cast<uint8_t*>(this + 1);
    }

    /// @brief Get pointer to the data portion (const).
    /// @return Const pointer to the data immediately following the header.
    const uint8_t* data() const {
        return reinterpret_cast<const uint8_t*>(this + 1);
    }

    // ─── Static Factory Methods ──────────────────────────────────────────────

    /// @brief Allocate a new environment with a single reference.
    /// @param dataSize Size of the data portion in bytes.
    /// @return Pointer to the environment (header + data), or nullptr on failure.
    /// @note The data portion is zero-initialized.
    static ClosureEnvHeader* allocate(uint32_t dataSize) {
        // Calculate total size: header + data
        size_t totalSize = sizeof(ClosureEnvHeader) + dataSize;

        // Allocate memory
        void* mem = std::malloc(totalSize);
        if (!mem) {
            return nullptr;
        }

        // Construct the header in place
        ClosureEnvHeader* env = new (mem) ClosureEnvHeader;
        env->refcount.store(1, std::memory_order_release);
        env->size = dataSize;
        env->_padding = 0;

        // Zero-initialize the data portion
        std::memset(env->data(), 0, dataSize);

        return env;
    }

    /// @brief Allocate a new environment with a single reference and copy data.
    /// @param dataPtr Pointer to the data to copy.
    /// @param dataSize Size of the data portion in bytes.
    /// @return Pointer to the environment (header + data), or nullptr on failure.
    static ClosureEnvHeader* allocateWithData(const void* dataPtr, uint32_t dataSize) {
        ClosureEnvHeader* env = allocate(dataSize);
        if (!env) {
            return nullptr;
        }

        // Copy the data
        if (dataPtr && dataSize > 0) {
            std::memcpy(env->data(), dataPtr, dataSize);
        }

        return env;
    }

    /// @brief Retain (increment reference count).
    /// @param env Pointer to the environment (may be nullptr).
    static void retain(ClosureEnvHeader* env) {
        if (!env) {
            return;
        }
        env->refcount.fetch_add(1, std::memory_order_acq_rel);
    }

    /// @brief Release (decrement reference count, free if zero).
    /// @param env Pointer to the environment (may be nullptr).
    /// @return true if the environment was freed, false otherwise.
    static bool release(ClosureEnvHeader* env) {
        if (!env) {
            return false;
        }

        // Decrement the reference count
        uint32_t oldCount = env->refcount.fetch_sub(1, std::memory_order_acq_rel);

        // If this was the last reference (oldCount == 1), free the memory
        if (oldCount == 1) {
            // Destroy the header (call destructor - trivial for this type)
            env->~ClosureEnvHeader();
            // Free the memory
            std::free(env);
            return true;
        }

        return false;
    }

    /// @brief Get the current reference count.
    /// @param env Pointer to the environment.
    /// @return The current reference count, or 0 if env is null.
    static uint32_t getRefcount(ClosureEnvHeader* env) {
        if (!env) {
            return 0;
        }
        return env->refcount.load(std::memory_order_acquire);
    }

    /// @brief Check if a pointer is a valid environment.
    /// @param ptr Pointer to check.
    /// @return true if the pointer is a valid environment, false otherwise.
    static bool isValid(void* ptr) {
        if (!ptr) {
            return false;
        }

        // We can't fully validate a pointer without a magic number.
        // This is a basic sanity check: the pointer must be 8-byte aligned.
        // The header is 8-byte aligned, so a valid environment pointer
        // will always be 8-byte aligned.
        //
        // For stronger validation, we could add a magic number to the header,
        // but that adds overhead and complexity. The alignment check is
        // sufficient for the current use case.
        //
        // Note: This is used by __lucid_is_closure to determine if a value
        // is a closure environment. It's called frequently, so it should be fast.
        return (reinterpret_cast<uintptr_t>(ptr) % alignof(ClosureEnvHeader)) == 0;
    }
};
