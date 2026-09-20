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
///   │  │  │  refcount: atomic<int64_t>    (8 bytes)                 │ │   │
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
/// The header is placed at the start, followed immediately by the data. The
/// block comes from `lucid::runtime::heapAlloc`, so it is zeroed and visible
/// to the leak report, and it is released with `heapFree` (functions.def,
/// rule 5).
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
/// (which follows it directly) is 16-byte aligned too. Its layout is
/// `lucid::abi::LucidClosureHeader` from `runtime-abi/lucid_abi.h`: the
/// static_asserts at the bottom of this file pin the two together. Captured values are
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

#include "RuntimeInternal.hpp"
#include "runtime-abi/lucid_abi.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

/// @brief Header for every closure environment.
///
/// This header is placed at the beginning of every closure environment
/// allocation. It contains the reference count and the drop function.
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
    std::atomic<lucid::abi::LucidI64> refcount;

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
    static ClosureEnvHeader* allocate(std::size_t dataSize, DropFn drop = nullptr) {
        // Calculate total size: header + data
        if (dataSize > SIZE_MAX - sizeof(ClosureEnvHeader)) {
            return nullptr;
        }
        std::size_t totalSize = sizeof(ClosureEnvHeader) + dataSize;

        // Allocate memory. heapAlloc returns zeroed memory, so the data
        // portion is already zero-initialized.
        void* mem = lucid::runtime::heapAlloc(totalSize);
        if (!mem) {
            return nullptr;
        }

        // Construct the header in place
        ClosureEnvHeader* env = new (mem) ClosureEnvHeader;
        env->refcount.store(1, std::memory_order_release);
        env->drop = drop;

        return env;
    }

    /// @brief Allocate a new environment with a single reference and copy data.
    /// @param dataPtr Pointer to the data to copy.
    /// @param dataSize Size of the data portion in bytes.
    /// @param drop Optional drop function run when the refcount reaches zero.
    /// @return Pointer to the environment (header + data), or nullptr on failure.
    static ClosureEnvHeader* allocateWithData(const void* dataPtr, std::size_t dataSize,
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
        lucid::abi::LucidI64 oldCount = env->refcount.fetch_sub(1, std::memory_order_acq_rel);

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
            lucid::runtime::heapFree(env);
            return true;
        }

        return false;
    }

    /// @brief Get the current reference count.
    /// @param env Pointer to the environment.
    /// @return The current reference count, or 0 if env is null.
    static lucid::abi::LucidI64 getRefcount(ClosureEnvHeader* env) {
        if (!env) {
            return 0;
        }
        return env->refcount.load(std::memory_order_acquire);
    }

};

// The compiler-generated code relies on this exact layout (data at +16).
static_assert(sizeof(ClosureEnvHeader) == 16, "ClosureEnvHeader must be 16 bytes");
static_assert(alignof(ClosureEnvHeader) == 16, "ClosureEnvHeader must be 16-byte aligned");

// The header must mirror the ABI's LucidClosureHeader field for field. The
// only difference is that the refcount is an atomic; the assert below pins
// that an atomic integer has the same size, so the layouts coincide.
static_assert(sizeof(std::atomic<lucid::abi::LucidI64>) == sizeof(lucid::abi::LucidI64),
              "atomic<LucidI64> must be the same size as LucidI64");
static_assert(std::atomic<lucid::abi::LucidI64>::is_always_lock_free,
              "the refcount must be lock-free");
static_assert(sizeof(ClosureEnvHeader) == sizeof(lucid::abi::LucidClosureHeader),
              "ClosureEnvHeader must be the size of LucidClosureHeader");
static_assert(offsetof(ClosureEnvHeader, refcount) == offsetof(lucid::abi::LucidClosureHeader, refcount),
              "ClosureEnvHeader.refcount must sit where LucidClosureHeader.refcount does");
static_assert(offsetof(ClosureEnvHeader, drop) == offsetof(lucid::abi::LucidClosureHeader, drop),
              "ClosureEnvHeader.drop must sit where LucidClosureHeader.drop does");