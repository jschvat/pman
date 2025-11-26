#pragma once

#include <atomic>

namespace pman {

class FutexRwMutex {
public:
    FutexRwMutex() = default;

    FutexRwMutex(const FutexRwMutex&) = delete;
    FutexRwMutex& operator=(const FutexRwMutex&) = delete;

    void lock_shared();
    bool try_lock_shared();
    void unlock_shared();

    void lock();
    bool try_lock();
    void unlock();

    bool try_upgrade();
    void downgrade();

private:
    static constexpr int kWriterHolding = 1 << 30;
    static constexpr int kWriterWaiting = 1 << 29;
    static constexpr int kReaderMask = (1 << 29) - 1;

    std::atomic<int> state_{0};
    std::atomic<int> writer_futex_{0};
    std::atomic<int> reader_futex_{0};
};

class FutexReadGuard {
public:
    explicit FutexReadGuard(FutexRwMutex& mutex) : mutex_(mutex) {
        mutex_.lock_shared();
    }

    ~FutexReadGuard() {
        mutex_.unlock_shared();
    }

    FutexReadGuard(const FutexReadGuard&) = delete;
    FutexReadGuard& operator=(const FutexReadGuard&) = delete;

private:
    FutexRwMutex& mutex_;
};

class FutexWriteGuard {
public:
    explicit FutexWriteGuard(FutexRwMutex& mutex) : mutex_(mutex) {
        mutex_.lock();
    }

    ~FutexWriteGuard() {
        mutex_.unlock();
    }

    FutexWriteGuard(const FutexWriteGuard&) = delete;
    FutexWriteGuard& operator=(const FutexWriteGuard&) = delete;

private:
    FutexRwMutex& mutex_;
};

}  // namespace pman
