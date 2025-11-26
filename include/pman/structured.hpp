#pragma once

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace pman {

namespace detail {

template <typename...>
inline constexpr bool always_false_v = false;

template <typename Fn, typename Tuple>
void invokeWithToken(Fn& fn, std::stop_token token, Tuple& tuple) {
    auto caller = [&](auto&&... unpack) {
        if constexpr (std::is_invocable_v<Fn&, std::stop_token, decltype(unpack)...>) {
            fn(token, std::forward<decltype(unpack)>(unpack)...);
        } else if constexpr (std::is_invocable_v<Fn&, decltype(unpack)...>) {
            fn(std::forward<decltype(unpack)>(unpack)...);
        } else if constexpr (std::is_invocable_v<Fn&, std::stop_token>) {
            fn(token);
        } else if constexpr (std::is_invocable_v<Fn&>) {
            fn();
        } else {
            static_assert(always_false_v<Fn>, "spawn requires callable accepting (stop_token, ...) or (...)");
        }
    };
    std::apply(caller, tuple);
}

}  // namespace detail

class TaskScope {
public:
    TaskScope() = default;
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;

    ~TaskScope() {
        cancel();
        if (!joined_) {
            try {
                join();
            } catch (...) {
                std::terminate();
            }
        }
    }

    template <typename Fn, typename... Args>
    void spawn(Fn&& fn, Args&&... args) {
        using BoundTuple = std::tuple<std::decay_t<Args>...>;
        auto bound = std::make_shared<BoundTuple>(std::forward<Args>(args)...);
        std::stop_token token = stopSource_.get_token();
        std::lock_guard<std::mutex> lock(mutex_);
        if (joined_) {
            throw std::logic_error("TaskScope already joined");
        }
        threads_.emplace_back([this,
                               fn = std::forward<Fn>(fn),
                               bound = std::move(bound),
                               token]() mutable {
            try {
                detail::invokeWithToken(fn, token, *bound);
            } catch (...) {
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!exception_) {
                        exception_ = std::current_exception();
                    }
                }
                stopSource_.request_stop();
            }
        });
    }

    void cancel() {
        stopSource_.request_stop();
    }

    std::stop_token token() const noexcept {
        return stopSource_.get_token();
    }

    void join() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            threads.swap(threads_);
        }
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        std::exception_ptr ex;
        bool throwLater = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (exception_ && !exceptionPropagated_) {
                ex = exception_;
                exceptionPropagated_ = true;
                throwLater = true;
            }
            exception_ = nullptr;
            joined_ = true;
        }
        if (throwLater && ex) {
            std::rethrow_exception(ex);
        }
    }

private:
    std::vector<std::thread> threads_;
    std::stop_source stopSource_;
    mutable std::mutex mutex_;
    std::exception_ptr exception_;
    bool joined_{false};
    bool exceptionPropagated_{false};
};

class DeadlineCancel {
public:
    explicit DeadlineCancel(std::chrono::steady_clock::duration timeout) {
        if (timeout.count() < 0) {
            return;
        }
        timer_ = std::thread([this, timeout]() {
            std::this_thread::sleep_for(timeout);
            source_.request_stop();
        });
    }

    ~DeadlineCancel() {
        if (timer_.joinable()) {
            timer_.join();
        }
    }

    std::stop_token token() const noexcept { return source_.get_token(); }

private:
    std::stop_source source_;
    std::thread timer_;
};

template <typename Callable>
decltype(auto) with_deadline(std::chrono::steady_clock::duration timeout, Callable&& callable) {
    DeadlineCancel cancel(timeout);
    return callable(cancel.token());
}

}  // namespace pman
