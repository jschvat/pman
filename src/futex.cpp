#include "pman/futex.hpp"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <thread>

namespace pman {
namespace {

void futex_wait_internal(std::atomic<int>* addr, int expected, const timespec* timeout = nullptr) {
    while (true) {
        int rc = syscall(SYS_futex,
            reinterpret_cast<int*>(addr),
            FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
            expected,
            timeout,
            nullptr,
            0);
        if (rc == 0 || errno == EAGAIN) {
            return;
        }
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "futex_wait");
        }
        if (timeout != nullptr) {
            return;
        }
    }
}

void futex_wake_internal(std::atomic<int>* addr, int count) {
    int rc = syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
        count,
        nullptr,
        nullptr,
        0);
    if (rc == -1) {
        throw std::system_error(errno, std::generic_category(), "futex_wake");
    }
}

}  // namespace

void FutexMutex::lock() {
    int expected = Unlocked;
    if (state_.compare_exchange_strong(expected, Locked, std::memory_order_acquire)) {
        return;
    }

    int spins = 1000;
    while (true) {
        while (spins-- > 0) {
            expected = state_.load(std::memory_order_relaxed);
            if (expected == Unlocked) {
                if (state_.compare_exchange_strong(expected, Locked, std::memory_order_acquire)) {
                    return;
                }
            }
            std::this_thread::yield();
        }

        expected = state_.exchange(Contended, std::memory_order_acquire);
        if (expected == Unlocked) {
            return;
        }

        futex_wait_internal(&state_, Contended);
        spins = 1000;
    }
}

bool FutexMutex::try_lock() {
    int expected = Unlocked;
    return state_.compare_exchange_strong(expected, Locked, std::memory_order_acquire);
}

void FutexMutex::unlock() {
    int prev = state_.exchange(Unlocked, std::memory_order_release);
    if (prev == Contended) {
        futex_wake_internal(&state_, 1);
    }
}

void FutexCondVar::wait(FutexMutex& mutex) {
    wait_impl(mutex, nullptr);
}

bool FutexCondVar::wait_impl(FutexMutex& mutex, const timespec* timeout) {
    int seq = seq_.load(std::memory_order_relaxed);
    mutex.unlock();
    futex_wait_internal(&seq_, seq, timeout);
    mutex.lock();
    return seq != seq_.load(std::memory_order_relaxed);
}

void FutexCondVar::notify_one() {
    seq_.fetch_add(1, std::memory_order_release);
    futex_wake_internal(&seq_, 1);
}

void FutexCondVar::notify_all() {
    seq_.fetch_add(1, std::memory_order_release);
    futex_wake_internal(&seq_, INT_MAX);
}

}  // namespace pman
