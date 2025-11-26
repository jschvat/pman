#pragma once

#include <atomic>
#include <cstdint>

namespace pman {

class FutexRecursiveMutex {
public:
    FutexRecursiveMutex() = default;

    FutexRecursiveMutex(const FutexRecursiveMutex&) = delete;
    FutexRecursiveMutex& operator=(const FutexRecursiveMutex&) = delete;

    void lock();
    bool try_lock();
    void unlock();

    [[nodiscard]] unsigned int recursion_depth() const noexcept;

private:
    std::atomic<pid_t> owner_{0};
    unsigned int recursion_count_{0};
    std::atomic<int> futex_state_{0};
};

class FutexRecursiveLockGuard {
public:
    explicit FutexRecursiveLockGuard(FutexRecursiveMutex& mutex) : mutex_(mutex) {
        mutex_.lock();
    }

    ~FutexRecursiveLockGuard() {
        mutex_.unlock();
    }

    FutexRecursiveLockGuard(const FutexRecursiveLockGuard&) = delete;
    FutexRecursiveLockGuard& operator=(const FutexRecursiveLockGuard&) = delete;

private:
    FutexRecursiveMutex& mutex_;
};

}  // namespace pman
