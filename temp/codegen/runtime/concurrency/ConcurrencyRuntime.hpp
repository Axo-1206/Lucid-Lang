/// @file ConcurrencyRuntime.hpp
/// @brief Concurrency runtime - thread pool, event loop, and future/thread management.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides the runtime support for Lucid's concurrency features:
///   - `async` / `await` - Cooperative concurrency (single-threaded event loop)
///   - `spawn` / `join` - Parallelism (OS thread pool)
///
/// Both `async` and `spawn` produce linear types that must be consumed exactly once.
///
/// ─── Memory Management ──────────────────────────────────────────────────────
/// FutureHandle and ThreadHandle are heap-allocated and reference-counted.
/// They are created by __lucid_async/__lucid_spawn and destroyed by
/// __lucid_await/__lucid_join or on shutdown.
///
/// ─── Thread Safety ──────────────────────────────────────────────────────────
/// The thread pool uses mutexes and condition variables for safe concurrent
/// access. The event loop is single-threaded and does not require synchronization
/// (tasks are scheduled from the main thread and executed on the main thread).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lucid::runtime {

// ─── Forward Declarations ──────────────────────────────────────────────────

struct FutureHandle;
struct ThreadHandle;

// ─── Handle States ─────────────────────────────────────────────────────────

/// @brief States for FutureHandle (async operations).
enum class FutureState : uint8_t {
    Pending = 0,    ///< Operation not yet started or still running
    Ready = 1,      ///< Operation completed, result available
    Consumed = 2,   ///< Result has been consumed (linear type)
    Error = 3       ///< Operation failed with an error
};

/// @brief States for ThreadHandle (spawn operations).
enum class ThreadState : uint8_t {
    Running = 0,    ///< Thread is still running
    Done = 1,       ///< Thread completed, result available
    Consumed = 2,   ///< Result has been consumed (linear type)
    Error = 3       ///< Thread failed with an error
};

// ─── FutureHandle ──────────────────────────────────────────────────────────

/// @brief Handle for an async operation (Future<T>).
///
/// This struct is heap-allocated and reference-counted. It tracks the state
/// of the async operation and the result once completed.
struct FutureHandle {
    std::atomic<uint64_t> refcount;   ///< Reference count
    std::atomic<FutureState> state;   ///< Current state (atomic for safe checking)
    void* result;                     ///< Result of the operation (owned)
    void* (*callable)(void*);         ///< The function to execute
    void* args;                       ///< Arguments for the callable

    /// @brief Allocate a new FutureHandle.
    static FutureHandle* allocate(void* (*callable)(void*), void* args);

    /// @brief Retain (increment reference count).
    static void retain(FutureHandle* handle);

    /// @brief Release (decrement reference count, free if zero).
    static void release(FutureHandle* handle);

    /// @brief Mark the future as ready with a result.
    void setReady(void* result);

    /// @brief Mark the future as failed with an error.
    void setError();

    /// @brief Check if the future is ready (non-blocking).
    bool isReady() const;
};

// ─── ThreadHandle ──────────────────────────────────────────────────────────

/// @brief Handle for a spawn operation (Thread<T>).
///
/// This struct is heap-allocated and reference-counted. It tracks the state
/// of the spawned thread and the result once completed.
struct ThreadHandle {
    std::atomic<uint64_t> refcount;   ///< Reference count
    std::atomic<ThreadState> state;   ///< Current state (atomic for safe checking)
    void* result;                     ///< Result of the thread (owned)
    void* (*callable)(void*);         ///< The function to execute
    void* args;                       ///< Arguments for the callable
    std::thread thread;               ///< The actual OS thread

    /// @brief Allocate a new ThreadHandle.
    static ThreadHandle* allocate(void* (*callable)(void*), void* args);

    /// @brief Retain (increment reference count).
    static void retain(ThreadHandle* handle);

    /// @brief Release (decrement reference count, free if zero).
    static void release(ThreadHandle* handle);

    /// @brief Mark the thread as done with a result.
    void setDone(void* result);

    /// @brief Mark the thread as failed with an error.
    void setError();

    /// @brief Check if the thread is done (non-blocking).
    bool isDone() const;

    /// @brief Wait for the thread to complete (blocks).
    void join();
};

// ─── Event Loop ────────────────────────────────────────────────────────────

/// @brief Cooperative event loop for async operations.
///
/// The event loop runs on the main thread and executes tasks in FIFO order.
/// Tasks are scheduled by __lucid_async and executed when the event loop runs.
class EventLoop {
public:
    /// @brief Get the singleton instance.
    static EventLoop& getInstance();

    /// @brief Schedule a task on the event loop.
    void schedule(FutureHandle* handle);

    /// @brief Run all pending tasks until the queue is empty.
    void runUntilEmpty();

    /// @brief Run a single task (returns early if none ready).
    void runOnce();

    /// @brief Get the number of pending tasks.
    size_t pendingCount() const;

    /// @brief Shutdown the event loop (clean up pending futures).
    void shutdown();

private:
    EventLoop() = default;
    ~EventLoop() = default;
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    std::queue<FutureHandle*> m_tasks;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_running{true};
};

// ─── Thread Pool ───────────────────────────────────────────────────────────

/// @brief Thread pool for spawn operations.
///
/// The thread pool manages a fixed number of OS threads that execute tasks
/// submitted by __lucid_spawn. Tasks are executed in parallel.
class ThreadPool {
public:
    /// @brief Get the singleton instance.
    static ThreadPool& getInstance();

    /// @brief Submit a task to the thread pool.
    void submit(ThreadHandle* handle);

    /// @brief Wait for all tasks to complete.
    void waitAll();

    /// @brief Shutdown the thread pool (wait for all tasks, then join threads).
    void shutdown();

    /// @brief Get the number of active threads.
    size_t activeCount() const;

private:
    ThreadPool();
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void workerLoop();

    std::vector<std::thread> m_workers;
    std::queue<ThreadHandle*> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{false};
    std::atomic<size_t> m_activeCount{0};
    size_t m_numWorkers;
};

// ─── Shutdown ──────────────────────────────────────────────────────────────

/// @brief Shutdown the entire concurrency runtime.
///
/// This must be called before program exit to ensure all threads are joined
/// and all resources are cleaned up. Called automatically from __lucid_shutdown.
void shutdownConcurrency();

} // namespace lucid::runtime