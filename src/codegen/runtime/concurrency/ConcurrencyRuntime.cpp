/// @file runtime/RuntimeFunctionRegistry.cpp
/// @brief Implementation of the runtime function registry.
///
/// ─── Concurrency ABI Note ──────────────────────────────────────────────────────
/// The async and spawn functions have a 3rd parameter that is a pointer to
/// storage (void**), NOT a pointer to the handle itself. This is because the
/// runtime needs to write the handle back into the binding's alloca.
///
/// The registry entry for Async and Spawn reflects this:
///   - 3rd parameter type: getPtrType(ctx.llvmCtx)  (i8*)
///   - At runtime, this is treated as a void** (pointer to storage)
///   - The runtime writes the handle into *((void**)arg3)
///
/// @see CodeGenStmt.cpp - lowerAsyncStmt(), lowerSpawnStmt()
/// @see ConcurrencyRuntime.cpp - __lucid_async(), __lucid_spawn()

#include "ConcurrencyRuntime.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

namespace lucid::runtime {

// ─── FutureHandle Implementation ──────────────────────────────────────────

FutureHandle* FutureHandle::allocate(void* (*callable)(void*), void* args) {
    FutureHandle* handle = new FutureHandle();
    handle->refcount.store(1, std::memory_order_release);
    handle->state.store(FutureState::Pending, std::memory_order_release);
    handle->result = nullptr;
    handle->callable = callable;
    handle->args = args;
    return handle;
}

void FutureHandle::retain(FutureHandle* handle) {
    if (handle) {
        handle->refcount.fetch_add(1, std::memory_order_acq_rel);
    }
}

void FutureHandle::release(FutureHandle* handle) {
    if (!handle) return;
    uint64_t oldCount = handle->refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (oldCount == 1) {
        // Last reference - free the memory
        delete handle;
    }
}

void FutureHandle::setReady(void* result) {
    this->result = result;
    this->state.store(FutureState::Ready, std::memory_order_release);
}

void FutureHandle::setError() {
    this->state.store(FutureState::Error, std::memory_order_release);
}

bool FutureHandle::isReady() const {
    FutureState s = this->state.load(std::memory_order_acquire);
    return s == FutureState::Ready || s == FutureState::Error;
}

// ─── ThreadHandle Implementation ──────────────────────────────────────────

ThreadHandle* ThreadHandle::allocate(void* (*callable)(void*), void* args) {
    ThreadHandle* handle = new ThreadHandle();
    handle->refcount.store(1, std::memory_order_release);
    handle->state.store(ThreadState::Running, std::memory_order_release);
    handle->result = nullptr;
    handle->callable = callable;
    handle->args = args;
    return handle;
}

void ThreadHandle::retain(ThreadHandle* handle) {
    if (handle) {
        handle->refcount.fetch_add(1, std::memory_order_acq_rel);
    }
}

void ThreadHandle::release(ThreadHandle* handle) {
    if (!handle) return;
    uint64_t oldCount = handle->refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (oldCount == 1) {
        // Last reference - free the memory
        delete handle;
    }
}

void ThreadHandle::setDone(void* result) {
    this->result = result;
    this->state.store(ThreadState::Done, std::memory_order_release);
}

void ThreadHandle::setError() {
    this->state.store(ThreadState::Error, std::memory_order_release);
}

bool ThreadHandle::isDone() const {
    ThreadState s = this->state.load(std::memory_order_acquire);
    return s == ThreadState::Done || s == ThreadState::Error;
}

void ThreadHandle::join() {
    if (this->thread.joinable()) {
        this->thread.join();
    }
}

// ─── EventLoop Implementation ─────────────────────────────────────────────

EventLoop& EventLoop::getInstance() {
    static EventLoop instance;
    return instance;
}

void EventLoop::schedule(FutureHandle* handle) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tasks.push(handle);
}

void EventLoop::runUntilEmpty() {
    while (m_running.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_tasks.empty()) {
            break;
        }
        FutureHandle* handle = m_tasks.front();
        m_tasks.pop();
        lock.unlock();

        if (!handle) continue;

        // ─── Execute the task ──────────────────────────────────────────────────
        // The callable takes the args and returns a result.
        // If the callable is null, the future is already ready.
        if (handle->callable) {
            void* result = handle->callable(handle->args);
            if (result) {
                handle->setReady(result);
            } else {
                handle->setError();
            }
        } else {
            // No callable - future was already ready
            handle->setReady(nullptr);
        }

        // Release the handle (the caller retains it, we drop our reference)
        FutureHandle::release(handle);
    }
}

void EventLoop::runOnce() {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_tasks.empty()) {
        return;
    }
    FutureHandle* handle = m_tasks.front();
    m_tasks.pop();
    lock.unlock();

    if (!handle) return;

    if (handle->callable) {
        void* result = handle->callable(handle->args);
        if (result) {
            handle->setReady(result);
        } else {
            handle->setError();
        }
    } else {
        handle->setReady(nullptr);
    }

    FutureHandle::release(handle);
}

size_t EventLoop::pendingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tasks.size();
}

void EventLoop::shutdown() {
    m_running.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_tasks.empty()) {
        FutureHandle* handle = m_tasks.front();
        m_tasks.pop();
        if (handle) {
            // Mark as error and release
            handle->setError();
            FutureHandle::release(handle);
        }
    }
}

// ─── ThreadPool Implementation ────────────────────────────────────────────

ThreadPool::ThreadPool() {
    // Determine the number of workers (hardware concurrency)
    m_numWorkers = std::thread::hardware_concurrency();
    if (m_numWorkers == 0) {
        m_numWorkers = 4;  // Fallback
    }
    // Cap at a reasonable limit
    if (m_numWorkers > 32) {
        m_numWorkers = 32;
    }

    // Start worker threads
    for (size_t i = 0; i < m_numWorkers; ++i) {
        m_workers.emplace_back(&ThreadPool::workerLoop, this);
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

ThreadPool& ThreadPool::getInstance() {
    static ThreadPool instance;
    return instance;
}

void ThreadPool::submit(ThreadHandle* handle) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push(handle);
    }
    m_cv.notify_one();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] {
            return m_stop.load(std::memory_order_acquire) || !m_tasks.empty();
        });

        if (m_stop.load(std::memory_order_acquire) && m_tasks.empty()) {
            break;
        }

        ThreadHandle* handle = m_tasks.front();
        m_tasks.pop();
        lock.unlock();

        if (!handle) continue;

        // ─── Execute the task ──────────────────────────────────────────────────
        m_activeCount.fetch_add(1, std::memory_order_acq_rel);

        if (handle->callable) {
            void* result = handle->callable(handle->args);
            if (result) {
                handle->setDone(result);
            } else {
                handle->setError();
            }
        } else {
            handle->setDone(nullptr);
        }

        m_activeCount.fetch_sub(1, std::memory_order_acq_rel);

        // Release the handle (the caller retains it, we drop our reference)
        ThreadHandle::release(handle);

        // Notify any waiters (join)
        m_cv.notify_all();
    }
}

void ThreadPool::waitAll() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] {
        return m_tasks.empty() && m_activeCount.load(std::memory_order_acquire) == 0;
    });
}

void ThreadPool::shutdown() {
    m_stop.store(true, std::memory_order_release);
    m_cv.notify_all();

    for (std::thread& t : m_workers) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_workers.clear();

    // Clean up any remaining tasks
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_tasks.empty()) {
        ThreadHandle* handle = m_tasks.front();
        m_tasks.pop();
        if (handle) {
            handle->setError();
            ThreadHandle::release(handle);
        }
    }
}

size_t ThreadPool::activeCount() const {
    return m_activeCount.load(std::memory_order_acquire);
}

// ─── Shutdown Implementation ──────────────────────────────────────────────

void shutdownConcurrency() {
    // ─── 1. Shutdown the event loop ──────────────────────────────────────────
    EventLoop::getInstance().shutdown();

    // ─── 2. Shutdown the thread pool ─────────────────────────────────────────
    ThreadPool::getInstance().shutdown();
}

} // namespace lucid::runtime