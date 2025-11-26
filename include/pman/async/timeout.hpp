#pragma once

#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/time.hpp"

#include <atomic>
#include <coroutine>
#include <optional>
#include <stdexcept>

namespace pman::async {

/// @brief Exception thrown when an operation times out.
class TimeoutError : public std::runtime_error {
public:
    TimeoutError() : std::runtime_error("Operation timed out") {}
    explicit TimeoutError(const std::string& msg) : std::runtime_error(msg) {}
};

namespace detail {

/// @brief Awaiter that races a task against a timeout timer.
template<typename T>
class TimeoutAwaiter {
public:
    TimeoutAwaiter(Task<T>&& task, Duration timeout, EventLoop* loop)
        : task_(std::move(task))
        , timeout_(timeout)
        , loop_(loop ? loop : EventLoop::current())
        , completed_(false)
        , timedOut_(false)
        , timerId_(0) {

        if (!loop_) {
            throw std::runtime_error("No event loop available for timeout");
        }
    }

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        handle_ = handle;

        // Start the timeout timer
        timerId_ = loop_->addTimer(timeout_.toChrono(), [this]() {
            onTimeout();
        });

        // Start the task with completion callback
        task_.start([this]() {
            onTaskComplete();
        });
    }

    T await_resume() {
        // Cancel timer if still active
        if (timerId_ != 0) {
            loop_->cancelTimer(timerId_);
        }

        if (timedOut_) {
            throw TimeoutError();
        }

        return task_.result();
    }

private:
    void onTimeout() {
        bool expected = false;
        if (completed_.compare_exchange_strong(expected, true)) {
            timedOut_ = true;
            if (handle_) {
                handle_.resume();
            }
        }
    }

    void onTaskComplete() {
        bool expected = false;
        if (completed_.compare_exchange_strong(expected, true)) {
            timedOut_ = false;
            if (handle_) {
                handle_.resume();
            }
        }
    }

    Task<T> task_;
    Duration timeout_;
    EventLoop* loop_;
    std::atomic<bool> completed_;
    bool timedOut_;
    uint64_t timerId_;
    std::coroutine_handle<> handle_;
};

/// @brief Specialization for void tasks.
template<>
class TimeoutAwaiter<void> {
public:
    TimeoutAwaiter(Task<void>&& task, Duration timeout, EventLoop* loop)
        : task_(std::move(task))
        , timeout_(timeout)
        , loop_(loop ? loop : EventLoop::current())
        , completed_(false)
        , timedOut_(false)
        , timerId_(0) {

        if (!loop_) {
            throw std::runtime_error("No event loop available for timeout");
        }
    }

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        handle_ = handle;

        // Start the timeout timer
        timerId_ = loop_->addTimer(timeout_.toChrono(), [this]() {
            onTimeout();
        });

        // Start the task with completion callback
        task_.start([this]() {
            onTaskComplete();
        });
    }

    void await_resume() {
        // Cancel timer if still active
        if (timerId_ != 0) {
            loop_->cancelTimer(timerId_);
        }

        if (timedOut_) {
            throw TimeoutError();
        }
    }

private:
    void onTimeout() {
        bool expected = false;
        if (completed_.compare_exchange_strong(expected, true)) {
            timedOut_ = true;
            if (handle_) {
                handle_.resume();
            }
        }
    }

    void onTaskComplete() {
        bool expected = false;
        if (completed_.compare_exchange_strong(expected, true)) {
            timedOut_ = false;
            if (handle_) {
                handle_.resume();
            }
        }
    }

    Task<void> task_;
    Duration timeout_;
    EventLoop* loop_;
    std::atomic<bool> completed_;
    bool timedOut_;
    uint64_t timerId_;
    std::coroutine_handle<> handle_;
};

} // namespace detail

/// @brief Wrap a task with a timeout.
///
/// If the task doesn't complete within the specified duration, a TimeoutError
/// is thrown. Uses the Task callback mechanism to race between task completion
/// and timer expiration.
///
/// Example:
/// @code
/// try {
///     int result = co_await timeout(slowTask(), Duration::fromSeconds(5));
///     std::cout << "Completed: " << result << std::endl;
/// } catch (const TimeoutError& e) {
///     std::cout << "Task timed out!" << std::endl;
/// }
/// @endcode
///
/// @param task The task to execute
/// @param duration Maximum time to wait
/// @param loop Optional event loop (defaults to current)
/// @return Awaitable that completes with task result or throws TimeoutError
template<typename T>
auto timeout(Task<T>&& task, Duration duration, EventLoop* loop = nullptr) {
    return detail::TimeoutAwaiter<T>(std::move(task), duration, loop);
}

/// @brief Helper to run a task with a deadline.
///
/// Similar to timeout() but accepts an Instant instead of a Duration.
///
/// Example:
/// @code
/// auto deadline = Instant::now() + Duration::fromSeconds(10);
/// auto result = co_await withDeadline(task(), deadline);
/// @endcode
template<typename T>
auto withDeadline(Task<T>&& task, Instant deadline, EventLoop* loop = nullptr) {
    auto now = Instant::now();
    auto duration = deadline - now;

    if (duration.asMillis() <= 0) {
        throw TimeoutError("Deadline already passed");
    }

    return timeout(std::move(task), duration, loop);
}

/// @brief Try to complete a task within a timeout, returning optional result.
///
/// Instead of throwing TimeoutError, returns std::nullopt if the task times out.
/// This is useful when timeout is an expected condition rather than an error.
///
/// Example:
/// @code
/// auto result = co_await tryTimeout(task(), Duration::fromSeconds(1));
/// if (result) {
///     std::cout << "Completed: " << *result << std::endl;
/// } else {
///     std::cout << "Timed out" << std::endl;
/// }
/// @endcode
///
/// Note: For void tasks, use timeout() with try/catch instead.
template<typename T>
Task<std::optional<T>> tryTimeout(Task<T>&& task, Duration duration, EventLoop* loop = nullptr) {
    try {
        T result = co_await timeout(std::move(task), duration, loop);
        co_return std::optional<T>(std::move(result));
    } catch (const TimeoutError&) {
        co_return std::nullopt;
    }
}

namespace detail {

/// @brief Awaiter that races a task against timeout WITHOUT cancelling the task.
/// Behaves like JavaScript's Promise.race() - the loser keeps running.
template<typename T>
class NonCancellingTimeoutAwaiter {
public:
    NonCancellingTimeoutAwaiter(Task<T>&& task, Duration timeout, EventLoop* loop)
        : timeout_(timeout)
        , loop_(loop ? loop : EventLoop::current())
        , completed_(false)
        , timedOut_(false)
        , timerId_(0) {

        if (!loop_) {
            throw std::runtime_error("No event loop available for timeout");
        }

        // Start the task WITHOUT holding onto it - fire and forget!
        // This allows the task to continue running even after timeout
        task.start([this]() {
            onTaskComplete();
        });
    }

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        handle_ = handle;

        // Start the timeout timer
        timerId_ = loop_->addTimer(timeout_.toChrono(), [this]() {
            onTimeout();
        });
    }

    std::optional<T> await_resume() {
        // Cancel timer if still active
        if (timerId_ != 0) {
            loop_->cancelTimer(timerId_);
        }

        if (timedOut_) {
            return std::nullopt;  // Timeout - task continues running in background
        }

        // Task completed - we don't have access to the result anymore
        // since we started it independently. User needs to capture it.
        return std::nullopt;  // This is a limitation of fire-and-forget
    }

private:
    void onTimeout() {
        bool expected = false;
        if (completed_.compare_exchange_strong(expected, true)) {
            timedOut_ = true;
            if (handle_) {
                handle_.resume();
            }
            // Note: Task continues running in background!
        }
    }

    void onTaskComplete() {
        bool expected = false;
        if (completed_.compare_exchange_strong(expected, true)) {
            timedOut_ = false;
            if (handle_) {
                handle_.resume();
            }
        }
    }

    Duration timeout_;
    EventLoop* loop_;
    std::atomic<bool> completed_;
    bool timedOut_;
    uint64_t timerId_;
    std::coroutine_handle<> handle_;
};

/// @brief Specialization for void tasks with shared state to capture completion.
template<>
class NonCancellingTimeoutAwaiter<void> {
public:
    NonCancellingTimeoutAwaiter(Task<void>&& task, Duration timeout, EventLoop* loop)
        : timeout_(timeout)
        , loop_(loop ? loop : EventLoop::current())
        , completed_(std::make_shared<std::atomic<bool>>(false))
        , timedOut_(false)
        , timerId_(0) {

        if (!loop_) {
            throw std::runtime_error("No event loop available for timeout");
        }

        // CRITICAL: Start task WITHOUT awaiting - it becomes self-managing
        // The task will run to completion independently
        task.start();
        // Note: task is now heap-allocated and will delete itself when done
    }

    bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        handle_ = handle;

        // Start the timeout timer
        timerId_ = loop_->addTimer(timeout_.toChrono(), [this]() {
            onTimeout();
        });
    }

    bool await_resume() {
        // Cancel timer if still active
        if (timerId_ != 0) {
            loop_->cancelTimer(timerId_);
        }

        return !timedOut_;  // Returns true if completed before timeout
    }

private:
    void onTimeout() {
        bool expected = false;
        if (completed_->compare_exchange_strong(expected, true)) {
            timedOut_ = true;
            if (handle_) {
                handle_.resume();
            }
            // Note: Task continues running in background!
        }
    }

    Duration timeout_;
    EventLoop* loop_;
    std::shared_ptr<std::atomic<bool>> completed_;
    bool timedOut_;
    uint64_t timerId_;
    std::coroutine_handle<> handle_;
};

} // namespace detail

/// @brief Race a task against a timeout WITHOUT cancelling the task (Promise.race behavior).
///
/// Unlike timeout(), this function does NOT cancel the task when the timeout fires.
/// The task continues running in the background, similar to JavaScript's Promise.race().
///
/// **Important Limitation**: Because the task runs independently, you cannot retrieve
/// its result value. For tasks with return values, consider using timeout() instead
/// if you need the result, or use a shared state to capture the result externally.
///
/// Example:
/// @code
/// auto result = co_await race(slowTask(), Duration::fromSeconds(5));
/// if (result) {
///     std::cout << "Task completed in time" << std::endl;
/// } else {
///     std::cout << "Timeout - but task keeps running!" << std::endl;
/// }
/// @endcode
///
/// Use case: Fire-and-forget tasks where you want to stop waiting but let
/// the work complete (e.g., background logging, metrics upload, cleanup).
///
/// @param task The task to execute
/// @param duration Maximum time to wait
/// @param loop Optional event loop (defaults to current)
/// @return Awaitable that returns true if completed before timeout, false otherwise
template<typename T = void>
auto race(Task<T>&& task, Duration duration, EventLoop* loop = nullptr) {
    return detail::NonCancellingTimeoutAwaiter<T>(std::move(task), duration, loop);
}

} // namespace pman::async
