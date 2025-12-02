#pragma once

#include "task.hpp"
#include "event_loop.hpp"
#include <vector>
#include <tuple>
#include <optional>
#include <variant>
#include <atomic>
#include <memory>

namespace pman::async {

/// Result of all() - contains results from all tasks
template<typename... Ts>
using AllResult = std::tuple<Ts...>;

/// Result of any() - contains the index and value of the first completed task
template<typename T>
struct AnyResult {
    size_t index;
    T value;
};

/// Result of first() - optional value (empty if no tasks provided)
template<typename T>
using FirstResult = std::optional<T>;

namespace detail {

/// Shared state for tracking completion of multiple tasks
template<typename T>
struct AllState {
    std::atomic<size_t> completed{0};
    std::atomic<bool> hasError{false};
    std::vector<std::optional<T>> results;
    std::exception_ptr error;
    std::function<void()> continuation;
    size_t total;

    explicit AllState(size_t n) : results(n), total(n) {}
};

/// Specialization for void - no results vector needed
template<>
struct AllState<void> {
    std::atomic<size_t> completed{0};
    std::atomic<bool> hasError{false};
    std::exception_ptr error;
    std::function<void()> continuation;
    size_t total;

    explicit AllState(size_t n) : total(n) {}
};

template<typename T>
struct AnyState {
    std::atomic<bool> done{false};
    std::optional<size_t> winnerIndex;
    std::optional<T> result;
    std::exception_ptr error;
    std::function<void()> continuation;

    AnyState() = default;
};

/// Specialization for void - no result needed
template<>
struct AnyState<void> {
    std::atomic<bool> done{false};
    std::optional<size_t> winnerIndex;
    std::exception_ptr error;
    std::function<void()> continuation;

    AnyState() = default;
};

/// Awaiter for all() combinator
template<typename T>
class AllAwaiter {
public:
    AllAwaiter(std::vector<Task<T>> tasks, EventLoop* loop)
        : tasks_(std::move(tasks))
        , loop_(loop)
        , state_(std::make_shared<AllState<T>>(tasks_.size())) {}

    bool await_ready() const noexcept {
        return tasks_.empty();
    }

    void await_suspend(std::coroutine_handle<> h) {
        state_->continuation = [h]() { h.resume(); };

        for (size_t i = 0; i < tasks_.size(); ++i) {
            size_t idx = i;
            auto state = state_;

            // IMPORTANT: Pass task as parameter, not captured in lambda!
            // Lambda captures live in the lambda object, which is destroyed after
            // the coroutine is created. Parameters become part of the coroutine frame.
            auto wrapper = [](std::shared_ptr<AllState<T>> state, size_t idx, Task<T> t) -> Task<void> {
                try {
                    if constexpr (std::is_void_v<T>) {
                        co_await std::move(t);
                    } else {
                        state->results[idx] = co_await std::move(t);
                    }
                } catch (...) {
                    bool expected = false;
                    if (state->hasError.compare_exchange_strong(expected, true)) {
                        state->error = std::current_exception();
                    }
                }

                if (++state->completed == state->total) {
                    if (state->continuation) {
                        state->continuation();
                    }
                }
            };

            wrapper(state, idx, std::move(tasks_[i])).start();
        }
    }

    std::vector<T> await_resume() {
        if (state_->error) {
            std::rethrow_exception(state_->error);
        }

        std::vector<T> results;
        results.reserve(tasks_.size());
        for (auto& opt : state_->results) {
            if (opt) {
                results.push_back(std::move(*opt));
            }
        }
        return results;
    }

private:
    std::vector<Task<T>> tasks_;
    EventLoop* loop_;
    std::shared_ptr<AllState<T>> state_;
};

/// Specialization for void tasks
template<>
class AllAwaiter<void> {
public:
    AllAwaiter(std::vector<Task<void>> tasks, EventLoop* loop)
        : tasks_(std::move(tasks))
        , loop_(loop)
        , state_(std::make_shared<AllState<void>>(tasks_.size())) {}

    bool await_ready() const noexcept {
        return tasks_.empty();
    }

    void await_suspend(std::coroutine_handle<> h) {
        state_->continuation = [h]() { h.resume(); };

        for (size_t i = 0; i < tasks_.size(); ++i) {
            size_t idx = i;
            auto state = state_;

            // Pass task as parameter, not captured in lambda
            auto wrapper = [](std::shared_ptr<AllState<void>> state, size_t idx, Task<void> t) -> Task<void> {
                try {
                    co_await std::move(t);
                } catch (...) {
                    bool expected = false;
                    if (state->hasError.compare_exchange_strong(expected, true)) {
                        state->error = std::current_exception();
                    }
                }

                if (++state->completed == state->total) {
                    if (state->continuation) {
                        state->continuation();
                    }
                }
            };

            wrapper(state, idx, std::move(tasks_[i])).start();
        }
    }

    void await_resume() {
        if (state_->error) {
            std::rethrow_exception(state_->error);
        }
    }

private:
    std::vector<Task<void>> tasks_;
    EventLoop* loop_;
    std::shared_ptr<AllState<void>> state_;
};

/// Awaiter for any() combinator - returns when first task completes
template<typename T>
class AnyAwaiter {
public:
    AnyAwaiter(std::vector<Task<T>> tasks, EventLoop* loop)
        : tasks_(std::move(tasks))
        , loop_(loop)
        , state_(std::make_shared<AnyState<T>>()) {}

    bool await_ready() const noexcept {
        return tasks_.empty();
    }

    void await_suspend(std::coroutine_handle<> h) {
        state_->continuation = [h]() { h.resume(); };

        for (size_t i = 0; i < tasks_.size(); ++i) {
            size_t idx = i;
            auto state = state_;

            // Pass task as parameter, not captured in lambda
            auto wrapper = [](std::shared_ptr<AnyState<T>> state, size_t idx, Task<T> t) -> Task<void> {
                try {
                    if constexpr (std::is_void_v<T>) {
                        co_await std::move(t);
                        bool expected = false;
                        if (state->done.compare_exchange_strong(expected, true)) {
                            state->winnerIndex = idx;
                            if (state->continuation) {
                                state->continuation();
                            }
                        }
                    } else {
                        auto result = co_await std::move(t);
                        bool expected = false;
                        if (state->done.compare_exchange_strong(expected, true)) {
                            state->winnerIndex = idx;
                            state->result = std::move(result);
                            if (state->continuation) {
                                state->continuation();
                            }
                        }
                    }
                } catch (...) {
                    bool expected = false;
                    if (state->done.compare_exchange_strong(expected, true)) {
                        state->error = std::current_exception();
                        if (state->continuation) {
                            state->continuation();
                        }
                    }
                }
            };

            wrapper(state, idx, std::move(tasks_[i])).start();
        }
    }

    AnyResult<T> await_resume() {
        if (state_->error) {
            std::rethrow_exception(state_->error);
        }

        return AnyResult<T>{
            state_->winnerIndex.value_or(0),
            std::move(*state_->result)
        };
    }

private:
    std::vector<Task<T>> tasks_;
    EventLoop* loop_;
    std::shared_ptr<AnyState<T>> state_;
};

/// Specialization for void tasks
template<>
class AnyAwaiter<void> {
public:
    AnyAwaiter(std::vector<Task<void>> tasks, EventLoop* loop)
        : tasks_(std::move(tasks))
        , loop_(loop)
        , state_(std::make_shared<AnyState<void>>()) {}

    bool await_ready() const noexcept {
        return tasks_.empty();
    }

    void await_suspend(std::coroutine_handle<> h) {
        state_->continuation = [h]() { h.resume(); };

        for (size_t i = 0; i < tasks_.size(); ++i) {
            size_t idx = i;
            auto state = state_;

            // Pass task as parameter, not captured in lambda
            auto wrapper = [](std::shared_ptr<AnyState<void>> state, size_t idx, Task<void> t) -> Task<void> {
                try {
                    co_await std::move(t);
                    bool expected = false;
                    if (state->done.compare_exchange_strong(expected, true)) {
                        state->winnerIndex = idx;
                        if (state->continuation) {
                            state->continuation();
                        }
                    }
                } catch (...) {
                    bool expected = false;
                    if (state->done.compare_exchange_strong(expected, true)) {
                        state->error = std::current_exception();
                        if (state->continuation) {
                            state->continuation();
                        }
                    }
                }
            };

            wrapper(state, idx, std::move(tasks_[i])).start();
        }
    }

    size_t await_resume() {
        if (state_->error) {
            std::rethrow_exception(state_->error);
        }
        return state_->winnerIndex.value_or(0);
    }

private:
    std::vector<Task<void>> tasks_;
    EventLoop* loop_;
    std::shared_ptr<AnyState<void>> state_;
};

} // namespace detail

/// Wait for all tasks to complete and return their results
/// @param tasks Vector of tasks to wait for
/// @param loop Event loop to use (optional, uses current if nullptr)
/// @return Vector of results from all tasks
/// @throws Rethrows the first exception if any task fails
template<typename T>
detail::AllAwaiter<T> all(std::vector<Task<T>> tasks, EventLoop* loop = nullptr) {
    if (!loop) {
        loop = EventLoop::current();
    }
    return detail::AllAwaiter<T>(std::move(tasks), loop);
}

/// Wait for any task to complete and return its result
/// @param tasks Vector of tasks to race
/// @param loop Event loop to use (optional, uses current if nullptr)
/// @return AnyResult containing the index and value of the winner
/// @throws Rethrows the first exception if the winning task fails
template<typename T>
detail::AnyAwaiter<T> any(std::vector<Task<T>> tasks, EventLoop* loop = nullptr) {
    if (!loop) {
        loop = EventLoop::current();
    }
    return detail::AnyAwaiter<T>(std::move(tasks), loop);
}

/// Wait for the first task to complete (same as any, but returns just the value)
/// @param tasks Vector of tasks to race
/// @param loop Event loop to use (optional, uses current if nullptr)
/// @return The result of the first completed task
template<typename T>
Task<T> first(std::vector<Task<T>> tasks, EventLoop* loop = nullptr) {
    auto result = co_await any(std::move(tasks), loop);
    co_return std::move(result.value);
}

/// Overload for void tasks
inline Task<void> first(std::vector<Task<void>> tasks, EventLoop* loop = nullptr) {
    co_await any(std::move(tasks), loop);
}

} // namespace pman::async
