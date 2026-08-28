/// @file ConcurrencyEntry.cpp
/// @brief Extern "C" entry points for the Lucid concurrency runtime.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the extern "C" functions that are called by
/// JIT-compiled and AOT-compiled Lucid code. These functions are declared
/// in RuntimeFunctionRegistry.hpp and called via LLVM IR calls.
///
/// ─── Important ──────────────────────────────────────────────────────────────
/// These functions MUST be exported from the binary (lucid.exe or game.exe)
/// so that JIT-compiled code can find them.
///
/// ─── ABI Stability ──────────────────────────────────────────────────────────
/// These functions form a stable ABI between the compiler and the runtime.
/// Changing their signatures requires updating both the compiler and the
/// runtime implementation.

#include "ConcurrencyRuntime.hpp"
#include <cstdint>
#include <cstddef>

extern "C" {

// ─── Async / Await ──────────────────────────────────────────────────────────

/// @brief Schedule a function on the event loop.
/// @param callable The function to execute (void* -> void*).
/// @param args Arguments for the callable.
/// @param future_handle_ptr Pointer to the FutureHandle* to store in.
/// @return The FutureHandle* (same as future_handle_ptr after assignment).
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenStmt.cpp in lowerAsyncStmt() when an async statement
/// is encountered. The callable is not executed immediately - it is scheduled
/// on the event loop and will be executed when the event loop runs.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// async result int = fetchData(url)
///   → __lucid_async(fetchData, url, &result_future)
void* __lucid_async(void* callable, void* args, void* future_handle_ptr) {
    if (!callable || !future_handle_ptr) {
        return nullptr;
    }

    // ─── 1. Allocate the future handle ──────────────────────────────────────
    void* (*fn)(void*) = reinterpret_cast<void* (*)(void*)>(callable);
    lucid::runtime::FutureHandle* handle =
        lucid::runtime::FutureHandle::allocate(fn, args);

    if (!handle) {
        return nullptr;
    }

    // ─── 2. Schedule on the event loop ──────────────────────────────────────
    lucid::runtime::EventLoop::getInstance().schedule(handle);

    // ─── 3. Store the handle in the caller's pointer ────────────────────────
    // The caller passes a pointer to a FutureHandle* (the alloca).
    // We store the handle there so the binding can access it.
    lucid::runtime::FutureHandle** futurePtr =
        static_cast<lucid::runtime::FutureHandle**>(future_handle_ptr);
    *futurePtr = handle;

    // Retain for the caller's reference
    lucid::runtime::FutureHandle::retain(handle);

    return handle;
}

/// @brief Block until a future is ready.
/// @param future_handle_ptr Pointer to the FutureHandle* to await.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenStmt.cpp in lowerAwaitStmt() when an await statement
/// is encountered. This function blocks the current thread until the future
/// is ready.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// await result
///   → __lucid_await(&result_future)
///
/// After this call, the future is marked as consumed and cannot be awaited again.
void __lucid_await(void* future_handle_ptr) {
    if (!future_handle_ptr) {
        return;
    }

    lucid::runtime::FutureHandle** futurePtr =
        static_cast<lucid::runtime::FutureHandle**>(future_handle_ptr);
    lucid::runtime::FutureHandle* handle = *futurePtr;

    if (!handle) {
        return;
    }

    // ─── 1. Check if already consumed ────────────────────────────────────────
    lucid::runtime::FutureState state = handle->state.load(std::memory_order_acquire);
    if (state == lucid::runtime::FutureState::Consumed) {
        // Double await - linear type violation
        // In a real implementation, we would panic here
        return;
    }

    // ─── 2. Wait until ready ──────────────────────────────────────────────────
    // In a cooperative system, we would yield to the event loop here.
    // For now, we spin with a small sleep (for demonstration).
    while (state == lucid::runtime::FutureState::Pending) {
        // Process the event loop to make progress
        lucid::runtime::EventLoop::getInstance().runOnce();
        state = handle->state.load(std::memory_order_acquire);
    }

    // ─── 3. Check for errors ──────────────────────────────────────────────────
    if (state == lucid::runtime::FutureState::Error) {
        // In a real implementation, we would panic here
        return;
    }

    // ─── 4. Mark as consumed ──────────────────────────────────────────────────
    handle->state.store(lucid::runtime::FutureState::Consumed, std::memory_order_release);

    // ─── 5. Release our reference ────────────────────────────────────────────
    lucid::runtime::FutureHandle::release(handle);
    *futurePtr = nullptr;
}

// ─── Spawn / Join ───────────────────────────────────────────────────────────

/// @brief Spawn a function on the thread pool.
/// @param callable The function to execute (void* -> void*).
/// @param args Arguments for the callable.
/// @param thread_handle_ptr Pointer to the ThreadHandle* to store in.
/// @return The ThreadHandle* (same as thread_handle_ptr after assignment).
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenStmt.cpp in lowerSpawnStmt() when a spawn statement
/// is encountered. The callable is submitted to the thread pool and will
/// execute on a separate OS thread.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// spawn result int = computeHeavyData()
///   → __lucid_spawn(computeHeavyData, &result_thread)
void* __lucid_spawn(void* callable, void* args, void* thread_handle_ptr) {
    if (!callable || !thread_handle_ptr) {
        return nullptr;
    }

    // ─── 1. Allocate the thread handle ──────────────────────────────────────
    void* (*fn)(void*) = reinterpret_cast<void* (*)(void*)>(callable);
    lucid::runtime::ThreadHandle* handle =
        lucid::runtime::ThreadHandle::allocate(fn, args);

    if (!handle) {
        return nullptr;
    }

    // ─── 2. Submit to thread pool ────────────────────────────────────────────
    lucid::runtime::ThreadPool::getInstance().submit(handle);

    // ─── 3. Store the handle in the caller's pointer ────────────────────────
    lucid::runtime::ThreadHandle** threadPtr =
        static_cast<lucid::runtime::ThreadHandle**>(thread_handle_ptr);
    *threadPtr = handle;

    // Retain for the caller's reference
    lucid::runtime::ThreadHandle::retain(handle);

    return handle;
}

/// @brief Block until a thread is complete.
/// @param thread_handle_ptr Pointer to the ThreadHandle* to join.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenStmt.cpp in lowerJoinStmt() when a join statement
/// is encountered. This function blocks the current thread until the
/// spawned thread completes.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// join result
///   → __lucid_join(&result_thread)
///
/// After this call, the thread is marked as consumed and cannot be joined again.
void __lucid_join(void* thread_handle_ptr) {
    if (!thread_handle_ptr) {
        return;
    }

    lucid::runtime::ThreadHandle** threadPtr =
        static_cast<lucid::runtime::ThreadHandle**>(thread_handle_ptr);
    lucid::runtime::ThreadHandle* handle = *threadPtr;

    if (!handle) {
        return;
    }

    // ─── 1. Check if already consumed ────────────────────────────────────────
    lucid::runtime::ThreadState state = handle->state.load(std::memory_order_acquire);
    if (state == lucid::runtime::ThreadState::Consumed) {
        // Double join - linear type violation
        return;
    }

    // ─── 2. Wait until done ──────────────────────────────────────────────────
    if (state == lucid::runtime::ThreadState::Running) {
        // Join the thread (blocks until complete)
        handle->join();
        state = handle->state.load(std::memory_order_acquire);
    }

    // ─── 3. Check for errors ──────────────────────────────────────────────────
    if (state == lucid::runtime::ThreadState::Error) {
        return;
    }

    // ─── 4. Mark as consumed ──────────────────────────────────────────────────
    handle->state.store(lucid::runtime::ThreadState::Consumed, std::memory_order_release);

    // ─── 5. Release our reference ────────────────────────────────────────────
    lucid::runtime::ThreadHandle::release(handle);
    *threadPtr = nullptr;
}

// ─── Shutdown ──────────────────────────────────────────────────────────────

/// @brief Shutdown the entire concurrency runtime.
///
/// ─── Usage ──────────────────────────────────────────────────────────────────
/// Called from CodeGenStmt.cpp in lowerReturnStmt() when the main function
/// returns. This ensures all threads are joined and all resources are cleaned up.
///
/// ─── Example ──────────────────────────────────────────────────────────────
/// // At program exit:
/// __lucid_shutdown()
void __lucid_shutdown() {
    lucid::runtime::shutdownConcurrency();
}

} // extern "C"