#pragma once

#include <atomic>
#include <chrono>

namespace pman {

class SharedFutexMutex {
public:
    SharedFutexMutex();

    SharedFutexMutex(const SharedFutexMutex&) = delete;
    SharedFutexMutex& operator=(const SharedFutexMutex&) = delete;

    void lock();
    bool try_lock();
    void unlock();

    void make_robust();
    [[nodiscard]] bool owner_dead() const noexcept;
    void consistent();

private:
    enum State : int { Unlocked = 0, Locked = 1, Contended = 2 };
    std::atomic<int> state_{Unlocked};
    std::atomic<pid_t> owner_{0};
    bool robust_{false};
    bool ownerDead_{false};
};

class SharedFutexCondVar {
public:
    SharedFutexCondVar();

    SharedFutexCondVar(const SharedFutexCondVar&) = delete;
    SharedFutexCondVar& operator=(const SharedFutexCondVar&) = delete;

    void wait(SharedFutexMutex& mutex);

    template <typename Rep, typename Period>
    bool wait_for(SharedFutexMutex& mutex, const std::chrono::duration<Rep, Period>& timeout);

    void notify_one();
    void notify_all();

private:
    bool wait_impl(SharedFutexMutex& mutex, const timespec* timeout);
    std::atomic<int> seq_{0};
};

class SharedFutexSemaphore {
public:
    explicit SharedFutexSemaphore(int initial_count = 0);

    SharedFutexSemaphore(const SharedFutexSemaphore&) = delete;
    SharedFutexSemaphore& operator=(const SharedFutexSemaphore&) = delete;

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

template <typename Rep, typename Period>
bool SharedFutexCondVar::wait_for(SharedFutexMutex& mutex,
                                   const std::chrono::duration<Rep, Period>& duration) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    timespec ts;
    ts.tv_sec = static_cast<time_t>(ns.count() / 1'000'000'000LL);
    ts.tv_nsec = static_cast<long>(ns.count() % 1'000'000'000LL);
    return wait_impl(mutex, &ts);
}

template <typename Rep, typename Period>
bool SharedFutexSemaphore::try_acquire_for(const std::chrono::duration<Rep, Period>& timeout) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout);
    timespec ts;
    ts.tv_sec = static_cast<time_t>(ns.count() / 1'000'000'000LL);
    ts.tv_nsec = static_cast<long>(ns.count() % 1'000'000'000LL);
    return try_acquire_impl(&ts);
}

}  // namespace pman
