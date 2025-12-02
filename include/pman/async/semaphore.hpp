#pragma once

#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"
#include <atomic>
#include <deque>
#include <coroutine>
#include <cstddef>

namespace pman::async {

/// @brief Async-aware semaphore for limiting concurrent access to resources
///
/// Semaphores control access to a limited number of resources. Tasks that
/// exceed the limit are suspended until resources become available.
///
/// Example:
/// @code
/// async::Semaphore sem(3);  // Allow 3 concurrent operations
///
/// auto task = [&]() -> Task<void> {
///     co_await sem.acquire();
///     // Access limited resource
///     co_await doWork();
///     sem.release();
/// };
/// @endcode
class Semaphore {
public:
    /// RAII guard that automatically releases the semaphore
    class Guard {
    public:
        Guard(Semaphore* sem) : sem_(sem) {}

        ~Guard() {
            if (sem_) {
                sem_->release();
            }
        }

        // Non-copyable but movable
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        Guard(Guard&& other) noexcept : sem_(other.sem_) {
            other.sem_ = nullptr;
        }

        Guard& operator=(Guard&& other) noexcept {
            if (this != &other) {
                if (sem_) {
                    sem_->release();
                }
                sem_ = other.sem_;
                other.sem_ = nullptr;
            }
            return *this;
        }

    private:
        Semaphore* sem_;
    };

    /// Awaiter that suspends until a permit is available
    class AcquireAwaiter {
    public:
        AcquireAwaiter(Semaphore* sem) : sem_(sem) {}

        bool await_ready() const noexcept {
            // Try to acquire immediately
            size_t current = sem_->count_.load(std::memory_order_acquire);
            while (current > 0) {
                if (sem_->count_.compare_exchange_weak(current, current - 1,
                    std::memory_order_acquire)) {
                    return true;
                }
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            sem_->waiters_.push_back(handle);
        }

        void await_resume() {}

    private:
        Semaphore* sem_;
    };

    /// Awaiter that returns a RAII guard
    class AcquireGuardAwaiter {
    public:
        AcquireGuardAwaiter(Semaphore* sem) : sem_(sem) {}

        bool await_ready() const noexcept {
            // Try to acquire immediately
            size_t current = sem_->count_.load(std::memory_order_acquire);
            while (current > 0) {
                if (sem_->count_.compare_exchange_weak(current, current - 1,
                    std::memory_order_acquire)) {
                    return true;
                }
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            sem_->waiters_.push_back(handle);
        }

        Guard await_resume() {
            return Guard(sem_);
        }

    private:
        Semaphore* sem_;
    };

    /// Create a semaphore with the given number of permits
    explicit Semaphore(size_t count) : count_(count), capacity_(count) {}

    /// Acquire a permit, suspending if none are available
    AcquireAwaiter acquire() {
        return AcquireAwaiter(this);
    }

    /// Acquire a permit and return a RAII guard
    AcquireGuardAwaiter acquireGuard() {
        return AcquireGuardAwaiter(this);
    }

    /// Try to acquire a permit without suspending
    /// @return true if successful, false otherwise
    bool tryAcquire() {
        size_t current = count_.load(std::memory_order_acquire);
        while (current > 0) {
            if (count_.compare_exchange_weak(current, current - 1,
                std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    /// Release a permit
    void release() {
        if (!waiters_.empty()) {
            // Wake up next waiter
            auto handle = waiters_.front();
            waiters_.pop_front();

            // Post to event loop to resume waiter
            auto loop = EventLoop::current();
            if (loop) {
                loop->post([handle]() { handle.resume(); });
            } else {
                // No event loop, resume directly
                handle.resume();
            }
        } else {
            // No waiters, increment count
            count_.fetch_add(1, std::memory_order_release);
        }
    }

    /// Get the current number of available permits
    size_t available() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

    /// Get the total capacity
    size_t capacity() const noexcept {
        return capacity_;
    }

private:
    std::atomic<size_t> count_;
    size_t capacity_;
    std::deque<std::coroutine_handle<>> waiters_;
};

} // namespace pman::async
