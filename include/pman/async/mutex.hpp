#pragma once

#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <coroutine>

namespace pman::async {

/// @brief Async-aware mutex that yields instead of blocking
///
/// Unlike std::mutex which blocks threads, async::Mutex suspends coroutines,
/// allowing the event loop to continue processing other tasks.
///
/// Example:
/// @code
/// async::Mutex mutex;
///
/// auto task = [&]() -> Task<void> {
///     auto lock = co_await mutex.lock();
///     // Critical section - lock is held
///     co_await doWork();
///     // Lock automatically released when 'lock' goes out of scope
/// };
/// @endcode
class Mutex {
public:
    /// RAII lock guard that automatically releases the mutex
    class LockGuard {
    public:
        LockGuard(Mutex* mutex) : mutex_(mutex) {}

        ~LockGuard() {
            if (mutex_) {
                mutex_->unlock();
            }
        }

        // Non-copyable but movable
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;

        LockGuard(LockGuard&& other) noexcept : mutex_(other.mutex_) {
            other.mutex_ = nullptr;
        }

        LockGuard& operator=(LockGuard&& other) noexcept {
            if (this != &other) {
                if (mutex_) {
                    mutex_->unlock();
                }
                mutex_ = other.mutex_;
                other.mutex_ = nullptr;
            }
            return *this;
        }

    private:
        Mutex* mutex_;
    };

    /// Awaiter that suspends until the mutex can be acquired
    class LockAwaiter {
    public:
        LockAwaiter(Mutex* mutex) : mutex_(mutex) {}

        bool await_ready() const noexcept {
            // Try to acquire immediately
            bool expected = false;
            return mutex_->locked_.compare_exchange_strong(expected, true,
                std::memory_order_acquire);
        }

        void await_suspend(std::coroutine_handle<> handle) {
            mutex_->waiters_.push_back(handle);
        }

        LockGuard await_resume() {
            return LockGuard(mutex_);
        }

    private:
        Mutex* mutex_;
    };

    Mutex() : locked_(false) {}

    /// Acquire the mutex, suspending if already locked
    /// @return LockGuard that automatically releases the mutex
    LockAwaiter lock() {
        return LockAwaiter(this);
    }

    /// Try to acquire the mutex without suspending
    /// @return LockGuard if successful, nullopt otherwise
    std::optional<LockGuard> tryLock() {
        bool expected = false;
        if (locked_.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
            return LockGuard(this);
        }
        return std::nullopt;
    }

    /// Check if the mutex is currently locked
    bool isLocked() const noexcept {
        return locked_.load(std::memory_order_acquire);
    }

private:
    void unlock() {
        if (!waiters_.empty()) {
            // Wake up next waiter
            auto handle = waiters_.front();
            waiters_.pop_front();

            // Post to event loop to resume waiter
            auto loop = EventLoop::current();
            if (loop) {
                loop->post([handle]() { handle.resume(); });
            } else {
                // No event loop, resume directly (shouldn't happen in practice)
                handle.resume();
            }
        } else {
            // No waiters, just unlock
            locked_.store(false, std::memory_order_release);
        }
    }

    std::atomic<bool> locked_;
    std::deque<std::coroutine_handle<>> waiters_;

    friend class LockGuard;
};

} // namespace pman::async
