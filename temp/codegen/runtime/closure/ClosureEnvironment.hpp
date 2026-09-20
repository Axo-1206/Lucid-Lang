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
///   │  ┌──────────────────────────────────────────────────────────────┐   │
///   │  │  ClosureEnvHeader (16 bytes)                                 │   │
///   │  │  ┌─────────────────────────────────────────────────────────┐ │   │
///   │  │  │  refcount: atomic<uint32_t>   (4 bytes)                 │ │   │
///   │  │  │  size: uint32_t               (4 bytes)                 │ │   │
///   │  │  │  drop: void(*)(void*)         (8 bytes, may be null)    │ │   │
///   │  │  └─────────────────────────────────────────────────────────┘ │   │
///   │  └──────────────────────────────────────────────────────────────┘   │
///   │  ┌──────────────────────────────────────────────────────────────┐   │
///   │  │  Data Portion (size bytes)                                   │   │
///   │  │  └── Captured variables stored contiguously                  │   │
///   │  └──────────────────────────────────────────────────────────────┘   │
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
/// - Release: decrements the count by 1; if it reaches 0, run the env's drop
///   function (if any) and then free the memory
///
/// ─── Drop Glue ───────────────────────────────────────────────────────────────
/// A by-value capture of a `cls` value retains that value's environment, so
/// the capturing environment owns one claim on it. Nothing in this header
/// knows the layout of the captured data, so the compiler emits a small
/// per-closure "drop" function that releases those claims, and passes it to
/// `__lucid_alloc_env`. It is stored in the header and invoked with a pointer
/// to the DATA portion when the refcount reaches zero. A null drop function
/// means the environment owns nothing that needs releasing.
///
/// ─── Alignment ───────────────────────────────────────────────────────────────
/// The header is exactly 16 bytes and 16-byte aligned, so the data portion
/// (which follows it directly) is 16-byte aligned too. Captured values are
/// LLVM structs that assume their natural alignment (pointers, i64, double,
/// { ptr, i64, i64 }); a 12-byte header would leave them misaligned by 4.
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
/// @note The header is 16 bytes and 16-byte aligned, so the data portion is
///       16-byte aligned (malloc returns 16-byte aligned blocks on the
///       supported 64-bit targets).
struct alignas(16) ClosureEnvHeader {
    /// Signature of the per-closure drop function emitted by the compiler.
    /// Receives a pointer to the data portion of the environment.
    using DropFn = void (*)(void* data);

    // ─── Fields ──────────────────────────────────────────────────────────────

    /// Reference count - number of references to this environment.
    /// Starts at 1. Protected by atomic operations for thread safety.
    std::atomic<uint32_t> refcount;

    /// Size of the data portion in bytes (does NOT include the header).
    uint32_t size;

    // ─── Drop glue ───────────────────────────────────────────────────────────
    // Releases whatever the captured data owns (e.g. retained closure
    // environments). Called exactly once, with data(), right before the
    // block is freed. May be null.
    DropFn drop;

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
    /// @param drop Optional drop function run when the refcount reaches zero.
    /// @return Pointer to the environment (header + data), or nullptr on failure.
    /// @note The data portion is zero-initialized.
    static ClosureEnvHeader* allocate(uint32_t dataSize, DropFn drop = nullptr) {
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
        env->drop = drop;

        // Zero-initialize the data portion
        std::memset(env->data(), 0, dataSize);

        return env;
    }

    /// @brief Allocate a new environment with a single reference and copy data.
    /// @param dataPtr Pointer to the data to copy.
    /// @param dataSize Size of the data portion in bytes.
    /// @param drop Optional drop function run when the refcount reaches zero.
    /// @return Pointer to the environment (header + data), or nullptr on failure.
    static ClosureEnvHeader* allocateWithData(const void* dataPtr, uint32_t dataSize,
                                              DropFn drop = nullptr) {
        ClosureEnvHeader* env = allocate(dataSize, drop);
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

    /// @brief Release (decrement reference count, drop and free if zero).
    /// @param env Pointer to the environment (may be nullptr).
    /// @return true if the environment was freed, false otherwise.
    static bool release(ClosureEnvHeader* env) {
        if (!env) {
            return false;
        }

        // Decrement the reference count
        uint32_t oldCount = env->refcount.fetch_sub(1, std::memory_order_acq_rel);

        // If this was the last reference (oldCount == 1), drop and free
        if (oldCount == 1) {
            // Release what the captured data owns (e.g. retained closure
            // environments) BEFORE the memory holding it goes away.
            if (env->drop) {
                env->drop(env->data());
            }
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

};

// The compiler-generated code relies on this exact layout (data at +16).
static_assert(sizeof(ClosureEnvHeader) == 16, "ClosureEnvHeader must be 16 bytes");
static_assert(alignof(ClosureEnvHeader) == 16, "ClosureEnvHeader must be 16-byte aligned");