#pragma once

#include <atomic>
#include <utility>

namespace pman {

class FutexOnceFlag {
public:
    FutexOnceFlag() = default;

    FutexOnceFlag(const FutexOnceFlag&) = delete;
    FutexOnceFlag& operator=(const FutexOnceFlag&) = delete;

    template <typename Callable, typename... Args>
    void call_once(Callable&& fn, Args&&... args);

    [[nodiscard]] bool has_run() const noexcept;

private:
    void wait_for_completion();
    void mark_done_and_wake();

    enum State : int { NotStarted = 0, Running = 1, Done = 2 };
    std::atomic<int> state_{NotStarted};
};

template <typename Callable, typename... Args>
void FutexOnceFlag::call_once(Callable&& fn, Args&&... args) {
    int expected = NotStarted;
    if (state_.compare_exchange_strong(expected, Running,
            std::memory_order_acquire, std::memory_order_relaxed)) {
        try {
            std::forward<Callable>(fn)(std::forward<Args>(args)...);
            mark_done_and_wake();
        } catch (...) {
            state_.store(NotStarted, std::memory_order_release);
            throw;
        }
        return;
    }

    if (expected == Done) {
        return;
    }

    wait_for_completion();
}

}  // namespace pman
