#pragma once

#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"
#include <atomic>
#include <deque>
#include <coroutine>

namespace pman::async {

/// @brief Async-aware reader-writer lock
///
/// Allows multiple concurrent readers OR a single writer. Readers and writers
/// suspend when the lock is not available in the desired mode.
///
/// Example:
/// @code
/// async::RwLock lock;
///
/// // Multiple readers can run concurrently
/// auto reader = [&]() -> Task<void> {
///     auto guard = co_await lock.readLock();
///     co_await readData();
/// };
///
/// // Writers have exclusive access
/// auto writer = [&]() -> Task<void> {
///     auto guard = co_await lock.writeLock();
///     co_await writeData();
/// };
/// @endcode
class RwLock {
public:
    /// RAII guard for read locks
    class ReadGuard {
    public:
        ReadGuard(RwLock* lock) : lock_(lock) {}

        ~ReadGuard() {
            if (lock_) {
                lock_->unlockRead();
            }
        }

        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        ReadGuard(ReadGuard&& other) noexcept : lock_(other.lock_) {
            other.lock_ = nullptr;
        }

        ReadGuard& operator=(ReadGuard&& other) noexcept {
            if (this != &other) {
                if (lock_) {
                    lock_->unlockRead();
                }
                lock_ = other.lock_;
                other.lock_ = nullptr;
            }
            return *this;
        }

    private:
        RwLock* lock_;
    };

    /// RAII guard for write locks
    class WriteGuard {
    public:
        WriteGuard(RwLock* lock) : lock_(lock) {}

        ~WriteGuard() {
            if (lock_) {
                lock_->unlockWrite();
            }
        }

        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

        WriteGuard(WriteGuard&& other) noexcept : lock_(other.lock_) {
            other.lock_ = nullptr;
        }

        WriteGuard& operator=(WriteGuard&& other) noexcept {
            if (this != &other) {
                if (lock_) {
                    lock_->unlockWrite();
                }
                lock_ = other.lock_;
                other.lock_ = nullptr;
            }
            return *this;
        }

    private:
        RwLock* lock_;
    };

    /// Awaiter for read locks
    class ReadLockAwaiter {
    public:
        ReadLockAwaiter(RwLock* lock) : lock_(lock) {}

        bool await_ready() const noexcept {
            // Can acquire read lock if no writers (state >= 0)
            int32_t expected = lock_->state_.load(std::memory_order_acquire);
            while (expected >= 0) {
                if (lock_->state_.compare_exchange_weak(expected, expected + 1,
                    std::memory_order_acquire)) {
                    return true;
                }
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            lock_->readWaiters_.push_back(handle);
        }

        ReadGuard await_resume() {
            return ReadGuard(lock_);
        }

    private:
        RwLock* lock_;
    };

    /// Awaiter for write locks
    class WriteLockAwaiter {
    public:
        WriteLockAwaiter(RwLock* lock) : lock_(lock) {}

        bool await_ready() const noexcept {
            // Can acquire write lock if no readers or writers (state == 0)
            int32_t expected = 0;
            return lock_->state_.compare_exchange_strong(expected, -1,
                std::memory_order_acquire);
        }

        void await_suspend(std::coroutine_handle<> handle) {
            lock_->writeWaiters_.push_back(handle);
        }

        WriteGuard await_resume() {
            return WriteGuard(lock_);
        }

    private:
        RwLock* lock_;
    };

    RwLock() : state_(0) {}

    /// Acquire a read lock (shared)
    /// Multiple readers can hold the lock concurrently
    ReadLockAwaiter readLock() {
        return ReadLockAwaiter(this);
    }

    /// Acquire a write lock (exclusive)
    /// Only one writer can hold the lock
    WriteLockAwaiter writeLock() {
        return WriteLockAwaiter(this);
    }

    /// Try to acquire a read lock without suspending
    std::optional<ReadGuard> tryReadLock() {
        int32_t expected = state_.load(std::memory_order_acquire);
        while (expected >= 0) {
            if (state_.compare_exchange_weak(expected, expected + 1,
                std::memory_order_acquire)) {
                return ReadGuard(this);
            }
        }
        return std::nullopt;
    }

    /// Try to acquire a write lock without suspending
    std::optional<WriteGuard> tryWriteLock() {
        int32_t expected = 0;
        if (state_.compare_exchange_strong(expected, -1, std::memory_order_acquire)) {
            return WriteGuard(this);
        }
        return std::nullopt;
    }

    /// Check if there are any active readers
    bool hasReaders() const noexcept {
        return state_.load(std::memory_order_acquire) > 0;
    }

    /// Check if there is an active writer
    bool hasWriter() const noexcept {
        return state_.load(std::memory_order_acquire) < 0;
    }

private:
    void unlockRead() {
        int32_t prev = state_.fetch_sub(1, std::memory_order_release);

        // If we were the last reader and there are waiting writers, wake one
        if (prev == 1 && !writeWaiters_.empty()) {
            // Try to acquire write lock for the waiter
            int32_t expected = 0;
            if (state_.compare_exchange_strong(expected, -1, std::memory_order_acquire)) {
                auto handle = writeWaiters_.front();
                writeWaiters_.pop_front();

                auto loop = EventLoop::current();
                if (loop) {
                    loop->post([handle]() { handle.resume(); });
                } else {
                    handle.resume();
                }
            }
        }
    }

    void unlockWrite() {
        state_.store(0, std::memory_order_release);

        // Prefer writers over readers (writer-priority)
        if (!writeWaiters_.empty()) {
            // Try to give lock to next writer
            int32_t expected = 0;
            if (state_.compare_exchange_strong(expected, -1, std::memory_order_acquire)) {
                auto handle = writeWaiters_.front();
                writeWaiters_.pop_front();

                auto loop = EventLoop::current();
                if (loop) {
                    loop->post([handle]() { handle.resume(); });
                } else {
                    handle.resume();
                }
            }
        } else if (!readWaiters_.empty()) {
            // Wake all waiting readers
            auto loop = EventLoop::current();
            std::deque<std::coroutine_handle<>> toWake;
            toWake.swap(readWaiters_);

            // Increment state for each reader we're waking
            state_.fetch_add(toWake.size(), std::memory_order_acquire);

            for (auto handle : toWake) {
                if (loop) {
                    loop->post([handle]() { handle.resume(); });
                } else {
                    handle.resume();
                }
            }
        }
    }

    // State:
    // > 0 = number of active readers
    // = 0 = unlocked
    // < 0 = writer locked
    std::atomic<int32_t> state_;

    std::deque<std::coroutine_handle<>> readWaiters_;
    std::deque<std::coroutine_handle<>> writeWaiters_;

    friend class ReadGuard;
    friend class WriteGuard;
};

} // namespace pman::async
