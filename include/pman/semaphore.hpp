#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace pman {

class FutexSemaphore {
public:
    explicit FutexSemaphore(int initial_count = 0);

    FutexSemaphore(const FutexSemaphore&) = delete;
    FutexSemaphore& operator=(const FutexSemaphore&) = delete;

    void acquire();
    bool try_acquire();

    template <typename Rep, typename Period>
    bool try_acquire_for(const std::chrono::duration<Rep, Period>& timeout);

    void release(int count = 1);

    [[nodiscard]] int count() const noexcept;

private:
    bool try_acquire_impl(const timespec* timeout);

    std::atomic<int> count_;
    std::atomic<int> waiters_{0};
};

using BinarySemaphore = FutexSemaphore;

template <typename Rep, typename Period>
bool FutexSemaphore::try_acquire_for(const std::chrono::duration<Rep, Period>& timeout) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
    timespec ts;
    ts.tv_sec = static_cast<time_t>(ns.count() / 1'000'000'000LL);
    ts.tv_nsec = static_cast<long>(ns.count() % 1'000'000'000LL);
    return try_acquire_impl(&ts);
}

}  // namespace pman
