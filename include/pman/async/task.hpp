#pragma once

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace pman::async {

class EventLoop;

namespace detail {
    // Helper to defer task deletion via event loop
    void deferTaskDeletion(void* task_ptr, void(*deleter)(void*));
}

template<typename T = void>
class Task;

namespace detail {
struct TrampolineGuard {
    // Prevents recursive coroutine resumption
};
} // namespace detail

template<typename T>
class Task {
public:
    struct promise_type {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation_) {
                    h.promise().continuation_();
                }
            }
            void await_resume() noexcept {}
        };

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T value) {
            result_ = std::move(value);
        }

        void unhandled_exception() {
            exception_ = std::current_exception();
        }

        std::optional<T> result_;
        std::exception_ptr exception_;
        std::function<void()> continuation_;
    };
    
    Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
    
    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }
    
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    void start(std::function<void()> on_complete = nullptr) {
        if (handle_) {
            if (on_complete) {
                handle_.promise().continuation_ = std::move(on_complete);
                handle_.resume();
            } else {
                // Top-level task: make it self-managing by heap-allocating
                // The task will delete itself when the coroutine completes
                Task* self = new Task(std::move(*this));
                self->handle_.promise().continuation_ = [self]() {
                    // CRITICAL: Must defer destruction to avoid destroying
                    // coroutine while still inside final_suspend.
                    detail::deferTaskDeletion(self, [](void* ptr) {
                        delete static_cast<Task*>(ptr);
                    });
                };
                self->handle_.resume();
            }
        }
    }

    T result() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
        return std::move(*handle_.promise().result_);
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> awaiter) {
        handle_.promise().continuation_ = [awaiter]() mutable {
            awaiter.resume();
        };
        handle_.resume();
    }

    T await_resume() {
        return result();
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

template<>
class Task<void> {
public:
    struct promise_type {
        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation_) {
                    h.promise().continuation_();
                }
            }
            void await_resume() noexcept {}
        };

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() { return {}; }
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() {}

        void unhandled_exception() {
            exception_ = std::current_exception();
        }

        std::exception_ptr exception_;
        std::function<void()> continuation_;
    };

    Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    void start(std::function<void()> on_complete = nullptr) {
        if (handle_) {
            if (on_complete) {
                handle_.promise().continuation_ = std::move(on_complete);
                handle_.resume();
            } else {
                // Top-level task: make it self-managing by heap-allocating
                // The task will delete itself when the coroutine completes
                Task* self = new Task(std::move(*this));
                self->handle_.promise().continuation_ = [self]() {
                    // CRITICAL: Must defer destruction to avoid destroying
                    // coroutine while still inside final_suspend.
                    detail::deferTaskDeletion(self, [](void* ptr) {
                        delete static_cast<Task*>(ptr);
                    });
                };
                self->handle_.resume();
            }
        }
    }

    void result() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }
    
    bool await_ready() const noexcept { return false; }
    
    void await_suspend(std::coroutine_handle<> awaiter) {
        handle_.promise().continuation_ = [awaiter]() mutable {
            awaiter.resume();
        };
        handle_.resume();
    }
    
    void await_resume() {
        result();
    }

private:
    std::coroutine_handle<promise_type> handle_;
};

} // namespace pman::async
