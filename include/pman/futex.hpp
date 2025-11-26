#pragma once

#include <atomic>
#include <chrono>
#include <climits>
#include <system_error>

namespace pman {

class FutexMutex {
public:
    FutexMutex() = default;
    FutexMutex(const FutexMutex&) = delete;
    FutexMutex& operator=(const FutexMutex&) = delete;

    void lock();
    bool try_lock();
    void unlock();

private:
    enum State : int { Unlocked = 0, Locked = 1, Contended = 2 };
    std::atomic<int> state_{Unlocked};
};

class FutexCondVar {
public:
    FutexCondVar() = default;

    void wait(FutexMutex& mutex);
    template <typename Rep, typename Period>
    bool wait_for(FutexMutex& mutex, const std::chrono::duration<Rep, Period>& duration) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
        timespec ts;
        ts.tv_sec = static_cast<time_t>(ns.count() / 1'000'000'000LL);
        ts.tv_nsec = static_cast<long>(ns.count() % 1'000'000'000LL);
        return wait_impl(mutex, &ts);
    }

    void notify_one();
    void notify_all();

private:
    bool wait_impl(FutexMutex& mutex, const timespec* timeout);
    std::atomic<int> seq_{0};
};

class FutexLockGuard {
public:
    explicit FutexLockGuard(FutexMutex& mutex) : mutex_(mutex) { mutex_.lock(); }
    ~FutexLockGuard() { mutex_.unlock(); }

    FutexLockGuard(const FutexLockGuard&) = delete;
    FutexLockGuard& operator=(const FutexLockGuard&) = delete;

private:
    FutexMutex& mutex_;
};

}  // namespace pman
