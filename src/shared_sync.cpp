#include "pman/shared_sync.hpp"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace pman {
namespace {

int shared_futex_wait(std::atomic<int>* addr, int expected, const timespec* timeout = nullptr) {
    return syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAIT,
        expected,
        timeout,
        nullptr,
        0);
}

void shared_futex_wake(std::atomic<int>* addr, int count) {
    syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAKE,
        count,
        nullptr,
        nullptr,
        0);
}

pid_t get_tid() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

}  // namespace

SharedFutexMutex::SharedFutexMutex() {}

void SharedFutexMutex::lock() {
    if (robust_ && ownerDead_) {
        throw std::runtime_error("SharedFutexMutex: owner died, call consistent() first");
    }

    pid_t self = get_tid();

    int expected = Unlocked;
    if (state_.compare_exchange_strong(expected, Locked, std::memory_order_acquire)) {
        if (robust_) {
            owner_.store(self, std::memory_order_relaxed);
        }
        return;
    }

    int spins = 1000;
    while (true) {
        while (spins-- > 0) {
            expected = state_.load(std::memory_order_relaxed);
            if (expected == Unlocked) {
                if (state_.compare_exchange_weak(expected, Locked, std::memory_order_acquire)) {
                    if (robust_) {
                        owner_.store(self, std::memory_order_relaxed);
                    }
                    return;
                }
            }
            std::this_thread::yield();
        }

        expected = state_.exchange(Contended, std::memory_order_acquire);
        if (expected == Unlocked) {
            if (robust_) {
                owner_.store(self, std::memory_order_relaxed);
            }
            return;
        }

        shared_futex_wait(&state_, Contended);
        spins = 1000;
    }
}

bool SharedFutexMutex::try_lock() {
    if (robust_ && ownerDead_) {
        return false;
    }

    int expected = Unlocked;
    if (state_.compare_exchange_strong(expected, Locked, std::memory_order_acquire)) {
        if (robust_) {
            owner_.store(get_tid(), std::memory_order_relaxed);
        }
        return true;
    }
    return false;
}

void SharedFutexMutex::unlock() {
    if (robust_) {
        owner_.store(0, std::memory_order_relaxed);
    }

    int prev = state_.exchange(Unlocked, std::memory_order_release);
    if (prev == Contended) {
        shared_futex_wake(&state_, 1);
    }
}

void SharedFutexMutex::make_robust() {
    robust_ = true;
}

bool SharedFutexMutex::owner_dead() const noexcept {
    return ownerDead_;
}

void SharedFutexMutex::consistent() {
    ownerDead_ = false;
    state_.store(Unlocked, std::memory_order_release);
    owner_.store(0, std::memory_order_relaxed);
}

SharedFutexCondVar::SharedFutexCondVar() {}

void SharedFutexCondVar::wait(SharedFutexMutex& mutex) {
    wait_impl(mutex, nullptr);
}

bool SharedFutexCondVar::wait_impl(SharedFutexMutex& mutex, const timespec* timeout) {
    int seq = seq_.load(std::memory_order_relaxed);
    mutex.unlock();

    int rc = shared_futex_wait(&seq_, seq, timeout);
    bool timedOut = (rc == -1 && errno == ETIMEDOUT);

    mutex.lock();
    return !timedOut;
}

void SharedFutexCondVar::notify_one() {
    seq_.fetch_add(1, std::memory_order_release);
    shared_futex_wake(&seq_, 1);
}

void SharedFutexCondVar::notify_all() {
    seq_.fetch_add(1, std::memory_order_release);
    shared_futex_wake(&seq_, std::numeric_limits<int>::max());
}

SharedFutexSemaphore::SharedFutexSemaphore(int initial_count) : count_(initial_count) {
    if (initial_count < 0) {
        throw std::invalid_argument("SharedFutexSemaphore: initial count cannot be negative");
    }
}

void SharedFutexSemaphore::acquire() {
    try_acquire_impl(nullptr);
}

bool SharedFutexSemaphore::try_acquire() {
    int current = count_.load(std::memory_order_relaxed);
    while (current > 0) {
        if (count_.compare_exchange_weak(current, current - 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool SharedFutexSemaphore::try_acquire_impl(const timespec* timeout) {
    while (true) {
        int current = count_.load(std::memory_order_relaxed);
        while (current > 0) {
            if (count_.compare_exchange_weak(current, current - 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return true;
            }
        }

        if (timeout != nullptr && timeout->tv_sec == 0 && timeout->tv_nsec == 0) {
            return false;
        }

        waiters_.fetch_add(1, std::memory_order_relaxed);

        current = count_.load(std::memory_order_relaxed);
        if (current > 0) {
            waiters_.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }

        int rc = shared_futex_wait(&count_, current, timeout);
        waiters_.fetch_sub(1, std::memory_order_relaxed);

        if (rc == -1) {
            if (errno == ETIMEDOUT) {
                return false;
            }
            if (errno != EAGAIN && errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "futex_wait");
            }
        }
    }
}

void SharedFutexSemaphore::release(int count) {
    if (count <= 0) {
        throw std::invalid_argument("SharedFutexSemaphore::release: count must be positive");
    }

    count_.fetch_add(count, std::memory_order_release);

    int to_wake = waiters_.load(std::memory_order_relaxed);
    if (to_wake > 0) {
        shared_futex_wake(&count_, std::min(count, to_wake));
    }
}

int SharedFutexSemaphore::count() const noexcept {
    return count_.load(std::memory_order_relaxed);
}

}  // namespace pman
