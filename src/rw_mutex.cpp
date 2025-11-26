#include "pman/rw_mutex.hpp"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <limits>
#include <system_error>
#include <thread>

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

void futex_wake_one(std::atomic<int>* addr) {
    syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
        1,
        nullptr,
        nullptr,
        0);
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

void FutexRwMutex::lock_shared() {
    while (true) {
        int state = state_.load(std::memory_order_relaxed);

        if ((state & (kWriterHolding | kWriterWaiting)) == 0) {
            if (state_.compare_exchange_weak(state, state + 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            continue;
        }

        int seq = reader_futex_.load(std::memory_order_relaxed);
        state = state_.load(std::memory_order_relaxed);
        if ((state & (kWriterHolding | kWriterWaiting)) != 0) {
            futex_wait(&reader_futex_, seq);
        }
    }
}

bool FutexRwMutex::try_lock_shared() {
    int state = state_.load(std::memory_order_relaxed);
    while (true) {
        if ((state & (kWriterHolding | kWriterWaiting)) != 0) {
            return false;
        }
        if (state_.compare_exchange_weak(state, state + 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void FutexRwMutex::unlock_shared() {
    int prev = state_.fetch_sub(1, std::memory_order_release);
    int readers = (prev & kReaderMask) - 1;

    if (readers == 0 && (prev & kWriterWaiting)) {
        writer_futex_.fetch_add(1, std::memory_order_release);
        futex_wake_one(&writer_futex_);
    }
}

void FutexRwMutex::lock() {
    while (true) {
        int state = state_.load(std::memory_order_relaxed);

        if (state == 0) {
            if (state_.compare_exchange_weak(state, kWriterHolding,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }
            continue;
        }

        if ((state & kWriterWaiting) == 0) {
            state_.fetch_or(kWriterWaiting, std::memory_order_relaxed);
        }

        int seq = writer_futex_.load(std::memory_order_relaxed);
        state = state_.load(std::memory_order_relaxed);

        if ((state & kReaderMask) != 0 || (state & kWriterHolding)) {
            futex_wait(&writer_futex_, seq);
            continue;
        }

        int expected = kWriterWaiting;
        if (state_.compare_exchange_strong(expected, kWriterHolding,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }
    }
}

bool FutexRwMutex::try_lock() {
    int expected = 0;
    return state_.compare_exchange_strong(expected, kWriterHolding,
        std::memory_order_acquire, std::memory_order_relaxed);
}

void FutexRwMutex::unlock() {
    state_.store(0, std::memory_order_release);

    reader_futex_.fetch_add(1, std::memory_order_release);
    futex_wake_all(&reader_futex_);

    writer_futex_.fetch_add(1, std::memory_order_release);
    futex_wake_one(&writer_futex_);
}

bool FutexRwMutex::try_upgrade() {
    int expected = 1;
    return state_.compare_exchange_strong(expected, kWriterHolding,
        std::memory_order_acquire, std::memory_order_relaxed);
}

void FutexRwMutex::downgrade() {
    state_.store(1, std::memory_order_release);

    reader_futex_.fetch_add(1, std::memory_order_release);
    futex_wake_all(&reader_futex_);
}

}  // namespace pman
