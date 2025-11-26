#include "pman/semaphore.hpp"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <limits>
#include <stdexcept>
#include <system_error>

namespace pman {
namespace {

int futex_wait(std::atomic<int>* addr, int expected, const timespec* timeout = nullptr) {
    return syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
        expected,
        timeout,
        nullptr,
        0);
}

void futex_wake(std::atomic<int>* addr, int count) {
    syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
        count,
        nullptr,
        nullptr,
        0);
}

}  // namespace

FutexSemaphore::FutexSemaphore(int initial_count) : count_(initial_count) {
    if (initial_count < 0) {
        throw std::invalid_argument("FutexSemaphore: initial count cannot be negative");
    }
}

void FutexSemaphore::acquire() {
    try_acquire_impl(nullptr);
}

bool FutexSemaphore::try_acquire() {
    int current = count_.load(std::memory_order_relaxed);
    while (current > 0) {
        if (count_.compare_exchange_weak(current, current - 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool FutexSemaphore::try_acquire_impl(const timespec* timeout) {
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

        int rc = futex_wait(&count_, current, timeout);
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

void FutexSemaphore::release(int count) {
    if (count <= 0) {
        throw std::invalid_argument("FutexSemaphore::release: count must be positive");
    }

    int prev = count_.fetch_add(count, std::memory_order_release);

    if (prev < 0) {
        throw std::logic_error("FutexSemaphore::release: semaphore count underflow");
    }

    int to_wake = waiters_.load(std::memory_order_relaxed);
    if (to_wake > 0) {
        futex_wake(&count_, std::min(count, to_wake));
    }
}

int FutexSemaphore::count() const noexcept {
    return count_.load(std::memory_order_relaxed);
}

}  // namespace pman
