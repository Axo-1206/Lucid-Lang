/// @file ConcurrencyEntry.cpp
/// @brief Extern "C" entry points for the Lucid concurrency runtime.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" functions that are called by
/// JIT-compiled and AOT-compiled Lucid code: the concurrency rows of
/// `runtime-abi/functions.def`. The contract for each one — what the thunk
/// and the packet are, what `out == null` means, who frees the result box —
/// is written down in that table; this file implements it.
///
/// ─── Signature Contract ───────────────────────────────────────────────────────
/// This file includes `runtime-abi/lucid_runtime.h`, so each definition below
/// is checked against its row at compile time. The rows tag the thunk, the
/// packet and the handle slot as `Ptr`, so they arrive as `void*`; the slot is
/// a `FutureHandle**` / `ThreadHandle**` and is cast inside.
///
/// ─── Important ──────────────────────────────────────────────────────────────
/// These functions MUST be reachable by JIT-compiled code (inside lucid.exe
/// for JIT mode, linked into game.exe for AOT mode).
///
/// ─── ABI Stability ──────────────────────────────────────────────────────────
/// These functions form a stable ABI between the compiler and the runtime.
/// Changing their signatures means changing their rows in functions.def.

#include "ConcurrencyRuntime.hpp"
#include "runtime-abi/lucid_runtime.h"

#include <cstdint>
#include <cstddef>

namespace {

using lucid::runtime::EventLoop;
using lucid::runtime::FutureHandle;
using lucid::runtime::FutureState;
using lucid::runtime::ThreadHandle;
using lucid::runtime::ThreadPool;
using lucid::runtime::ThreadState;

/// The compiler-emitted thunk: takes the argument packet, returns the boxed
/// result (or null on failure).
using Thunk = void* (*)(void*);

} // anonymous namespace

extern "C" {

// ─── Async / Await ──────────────────────────────────────────────────────────

/// @brief Queue a thunk on the cooperative event loop.
/// @param thunk   The compiler-emitted `void* (*)(void* packet)`.
/// @param packet  The heap argument packet; the thunk unpacks and frees it.
/// @param out     A `FutureHandle**` slot, or null.
///                - non-null: the handle is written into `*out`. That is the
///                  caller's reference; consume it with __lucid_await.
///                - null: fire and forget. The runtime keeps only its own
///                  reference and frees an unconsumed result box itself.
///
/// The task does not run here. It runs when something drives the event loop
/// (an `await`, or `EventLoop::runUntilEmpty`).
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// async result int = fetchData(url)
///   → __lucid_async(fetchData_thunk, packet, &result_future)
void __lucid_async(void* thunk, void* packet, void* out) {
    auto** slot = static_cast<FutureHandle**>(out);
    if (slot) {
        *slot = nullptr;   // a failure below must not leave a stale handle
    }
    if (!thunk) {
        return;
    }

    // The new handle carries one reference: the event loop's.
    FutureHandle* handle =
        FutureHandle::allocate(reinterpret_cast<Thunk>(thunk), packet);
    if (!handle) {
        return;
    }

    // Take the caller's reference BEFORE scheduling, so the handle cannot be
    // consumed and freed by the loop before the caller has claimed it.
    if (slot) {
        FutureHandle::retain(handle);
        *slot = handle;
    }

    EventLoop::getInstance().schedule(handle);
}

/// @brief Wait for a future and take its result.
/// @param handle_slot A `FutureHandle**` holding the handle from __lucid_async.
/// @return The boxed result, or null if the task failed or the slot was
///         already empty. The caller frees the box with __lucid_free.
///
/// Drives the event loop until the future is no longer pending, then
/// consumes the handle: `*handle_slot` is set to null, so a second await on
/// the same slot returns null (a linear-type violation Sema should already
/// have rejected).
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// await result
///   → result = __lucid_await(&result_future)
void* __lucid_await(void* handle_slot) {
    auto** slot = static_cast<FutureHandle**>(handle_slot);
    if (!slot || !*slot) {
        return nullptr;
    }
    FutureHandle* handle = *slot;

    // ─── 1. Wait until ready ─────────────────────────────────────────────────
    // A cooperative system: "waiting" means running queued tasks. If the
    // queue drains and the future is still pending, nothing can complete it.
    EventLoop& loop = EventLoop::getInstance();
    while (handle->state.load(std::memory_order_acquire) == FutureState::Pending) {
        if (loop.pendingCount() == 0) {
            break;
        }
        loop.runOnce();
    }

    // ─── 2. Take the result ──────────────────────────────────────────────────
    // Marking the future Consumed tells the final release that the result is
    // no longer the handle's to free.
    void* result = nullptr;
    if (handle->state.load(std::memory_order_acquire) == FutureState::Ready) {
        result = handle->result;
        handle->state.store(FutureState::Consumed, std::memory_order_release);
    }

    // ─── 3. Drop the caller's reference and clear the slot ───────────────────
    FutureHandle::release(handle);
    *slot = nullptr;
    return result;
}

// ─── Spawn / Join ───────────────────────────────────────────────────────────

/// @brief Submit a thunk to the thread pool.
/// @param thunk   The compiler-emitted `void* (*)(void* packet)`.
/// @param packet  The heap argument packet; the thunk unpacks and frees it.
/// @param out     A `ThreadHandle**` slot, or null (fire and forget); same
///                meaning as for __lucid_async.
///
/// The thunk runs on a pool worker, possibly before this function returns.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// spawn result int = computeHeavyData()
///   → __lucid_spawn(computeHeavyData_thunk, packet, &result_thread)
void __lucid_spawn(void* thunk, void* packet, void* out) {
    auto** slot = static_cast<ThreadHandle**>(out);
    if (slot) {
        *slot = nullptr;
    }
    if (!thunk) {
        return;
    }

    // The new handle carries one reference: the pool's.
    ThreadHandle* handle =
        ThreadHandle::allocate(reinterpret_cast<Thunk>(thunk), packet);
    if (!handle) {
        return;
    }

    // As in __lucid_async, but here it is not merely tidy: a worker can run
    // the task and drop the pool's reference before submit() returns. The
    // caller's reference must already exist by then.
    if (slot) {
        ThreadHandle::retain(handle);
        *slot = handle;
    }

    ThreadPool::getInstance().submit(handle);
}

/// @brief Wait for a spawned task and take its result.
/// @param handle_slot A `ThreadHandle**` holding the handle from __lucid_spawn.
/// @return The boxed result, or null if the task failed or the slot was
///         already empty. The caller frees the box with __lucid_free.
///
/// Blocks until the worker has finished the task, then consumes the handle:
/// `*handle_slot` is set to null.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// join result
///   → result = __lucid_join(&result_thread)
void* __lucid_join(void* handle_slot) {
    auto** slot = static_cast<ThreadHandle**>(handle_slot);
    if (!slot || !*slot) {
        return nullptr;
    }
    ThreadHandle* handle = *slot;

    // ─── 1. Wait until the worker is done ────────────────────────────────────
    handle->join();

    // ─── 2. Take the result ──────────────────────────────────────────────────
    void* result = nullptr;
    if (handle->state.load(std::memory_order_acquire) == ThreadState::Done) {
        result = handle->result;
        handle->state.store(ThreadState::Consumed, std::memory_order_release);
    }

    // ─── 3. Drop the caller's reference and clear the slot ───────────────────
    ThreadHandle::release(handle);
    *slot = nullptr;
    return result;
}

// ─── Shutdown ──────────────────────────────────────────────────────────────

/// @brief Shutdown the entire concurrency runtime.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Emitted by CodeGen at the end of `main` (see Abi::shutdownFn). This
/// ensures all threads are joined and all resources are cleaned up.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// // At program exit:
/// __lucid_shutdown()
void __lucid_shutdown() {
    lucid::runtime::shutdownConcurrency();
}

} // extern "C"