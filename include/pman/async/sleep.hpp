#pragma once

#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/time.hpp"

#include <coroutine>
#include <optional>

namespace pman::async {

namespace detail {

class SleepAwaiter {
public:
    explicit SleepAwaiter(Duration duration, EventLoop* loop = nullptr)
        : duration_(duration), loop_(loop) {}

    ~SleepAwaiter() {
        // Cancel timer if awaiter is destroyed before it fires
        if (timerId_ != 0 && eventLoop_) {
            eventLoop_->cancelTimer(timerId_);
        }
    }

    // Non-copyable, non-movable (awaiters are temporary)
    SleepAwaiter(const SleepAwaiter&) = delete;
    SleepAwaiter& operator=(const SleepAwaiter&) = delete;
    SleepAwaiter(SleepAwaiter&&) = delete;
    SleepAwaiter& operator=(SleepAwaiter&&) = delete;

    bool await_ready() const noexcept {
        // Never ready immediately - always suspend
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        // Get event loop (use provided or current)
        eventLoop_ = loop_ ? loop_ : EventLoop::current();
        if (!eventLoop_) {
            throw std::runtime_error("No event loop available for sleep");
        }

        // Schedule timer with completed flag protection
        timerId_ = eventLoop_->addTimer(duration_.toChrono(), [this, handle]() {
            completed_ = true;
            timerId_ = 0;  // Mark as fired
            handle.resume();
        });
    }

    void await_resume() {
        // Timer completed normally or was cancelled
    }

private:
    Duration duration_;
    EventLoop* loop_;
    EventLoop* eventLoop_{nullptr};
    uint64_t timerId_{0};
    bool completed_{false};
};

class SleepUntilAwaiter {
public:
    explicit SleepUntilAwaiter(Instant deadline, EventLoop* loop = nullptr)
        : deadline_(deadline), loop_(loop) {}

    ~SleepUntilAwaiter() {
        // Cancel timer if awaiter is destroyed before it fires
        if (timerId_ != 0 && eventLoop_) {
            eventLoop_->cancelTimer(timerId_);
        }
    }

    // Non-copyable, non-movable (awaiters are temporary)
    SleepUntilAwaiter(const SleepUntilAwaiter&) = delete;
    SleepUntilAwaiter& operator=(const SleepUntilAwaiter&) = delete;
    SleepUntilAwaiter(SleepUntilAwaiter&&) = delete;
    SleepUntilAwaiter& operator=(SleepUntilAwaiter&&) = delete;

    bool await_ready() const noexcept {
        // Check if deadline has already passed
        return deadline_.hasPassed();
    }

    void await_suspend(std::coroutine_handle<> handle) {
        // Get event loop (use provided or current)
        eventLoop_ = loop_ ? loop_ : EventLoop::current();
        if (!eventLoop_) {
            throw std::runtime_error("No event loop available for sleep_until");
        }

        // Calculate duration until deadline
        Duration duration = deadline_.durationUntil();

        // Schedule timer with completed flag protection
        timerId_ = eventLoop_->addTimer(duration.toChrono(), [this, handle]() {
            completed_ = true;
            timerId_ = 0;  // Mark as fired
            handle.resume();
        });
    }

    void await_resume() {
        // Timer completed normally or was cancelled
    }

private:
    Instant deadline_;
    EventLoop* loop_;
    EventLoop* eventLoop_{nullptr};
    uint64_t timerId_{0};
    bool completed_{false};
};

} // namespace detail

/// @brief Sleep for a specified duration.
///
/// Suspends the current task without blocking the thread. The task will
/// be resumed after at least the specified duration has elapsed.
///
/// Example:
/// @code
/// co_await sleep(Duration::fromSeconds(2));
/// co_await sleep(Duration::fromMillis(500));
/// @endcode
///
/// @param duration How long to sleep
/// @param loop Optional event loop (defaults to current)
/// @return Awaitable that completes after the duration
inline auto sleep(Duration duration, EventLoop* loop = nullptr) {
    return detail::SleepAwaiter(duration, loop);
}

/// @brief Sleep until a specific instant.
///
/// Suspends the current task until the specified instant is reached.
/// If the instant has already passed, returns immediately.
///
/// Example:
/// @code
/// Instant deadline = Instant::now() + Duration::fromSeconds(5);
/// co_await sleepUntil(deadline);
/// @endcode
///
/// @param deadline When to wake up
/// @param loop Optional event loop (defaults to current)
/// @return Awaitable that completes at the deadline
inline auto sleepUntil(Instant deadline, EventLoop* loop = nullptr) {
    return detail::SleepUntilAwaiter(deadline, loop);
}

/// @brief Yield control back to the event loop.
///
/// Allows other tasks to run without actually sleeping. Useful for
/// cooperative multitasking in CPU-bound loops.
///
/// Example:
/// @code
/// for (int i = 0; i < 1000000; ++i) {
///     doWork(i);
///     if (i % 1000 == 0) {
///         co_await yieldNow();  // Let other tasks run
///     }
/// }
/// @endcode
inline auto yieldNow() {
    return sleep(Duration::fromNanos(0));
}

} // namespace pman::async
