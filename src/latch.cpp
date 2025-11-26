#include "pman/latch.hpp"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <limits>
#include <stdexcept>
#include <system_error>

namespace pman {
namespace {

void futex_wait(std::atomic<int>* addr, int expected) {
    while (true) {
        int rc = syscall(SYS_futex,
            reinterpret_cast<int*>(addr),
            FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
            expected,
            nullptr,
            nullptr,
            0);
        if (rc == 0 || errno == EAGAIN) {
            return;
        }
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "futex_wait");
        }
    }
}

void futex_wake_all(std::atomic<int>* addr) {
    syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
        std::numeric_limits<int>::max(),
        nullptr,
        nullptr,
        0);
}

}  // namespace

FutexLatch::FutexLatch(std::ptrdiff_t expected) : counter_(expected) {
    if (expected < 0) {
        throw std::invalid_argument("FutexLatch: expected count cannot be negative");
    }
}

void FutexLatch::count_down(std::ptrdiff_t n) {
    if (n < 0) {
        throw std::invalid_argument("FutexLatch::count_down: n cannot be negative");
    }

    std::ptrdiff_t old = counter_.fetch_sub(n, std::memory_order_release);
    if (old == n) {
        futex_.fetch_add(1, std::memory_order_release);
        futex_wake_all(&futex_);
    } else if (old < n) {
        throw std::logic_error("FutexLatch::count_down: count underflow");
    }
}

bool FutexLatch::try_wait() const noexcept {
    return counter_.load(std::memory_order_acquire) <= 0;
}

void FutexLatch::wait() const {
    while (true) {
        if (counter_.load(std::memory_order_acquire) <= 0) {
            return;
        }

        int seq = futex_.load(std::memory_order_relaxed);
        if (counter_.load(std::memory_order_acquire) <= 0) {
            return;
        }

        futex_wait(&futex_, seq);
    }
}

void FutexLatch::arrive_and_wait(std::ptrdiff_t n) {
    count_down(n);
    wait();
}

}  // namespace pman
