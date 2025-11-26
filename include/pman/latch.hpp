#pragma once

#include <atomic>
#include <cstddef>

namespace pman {

class FutexLatch {
public:
    explicit FutexLatch(std::ptrdiff_t expected);

    FutexLatch(const FutexLatch&) = delete;
    FutexLatch& operator=(const FutexLatch&) = delete;

    void count_down(std::ptrdiff_t n = 1);
    [[nodiscard]] bool try_wait() const noexcept;
    void wait() const;
    void arrive_and_wait(std::ptrdiff_t n = 1);

private:
    std::atomic<std::ptrdiff_t> counter_;
    mutable std::atomic<int> futex_{0};
};

using CountdownLatch = FutexLatch;

}  // namespace pman
