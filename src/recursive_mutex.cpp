#include "pman/recursive_mutex.hpp"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>
#include <thread>

namespace pman {
namespace {

pid_t get_tid() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

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

constexpr int kUnlocked = 0;
constexpr int kLocked = 1;
constexpr int kContended = 2;

}  // namespace

void FutexRecursiveMutex::lock() {
    pid_t self = get_tid();

    if (owner_.load(std::memory_order_relaxed) == self) {
        ++recursion_count_;
        return;
    }

    int expected = kUnlocked;
    if (futex_state_.compare_exchange_strong(expected, kLocked,
            std::memory_order_acquire, std::memory_order_relaxed)) {
        owner_.store(self, std::memory_order_relaxed);
        recursion_count_ = 1;
        return;
    }

    int spins = 1000;
    while (true) {
        while (spins-- > 0) {
            expected = futex_state_.load(std::memory_order_relaxed);
            if (expected == kUnlocked) {
                if (futex_state_.compare_exchange_weak(expected, kLocked,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    owner_.store(self, std::memory_order_relaxed);
                    recursion_count_ = 1;
                    return;
                }
            }
            std::this_thread::yield();
        }

        expected = futex_state_.exchange(kContended, std::memory_order_acquire);
        if (expected == kUnlocked) {
            owner_.store(self, std::memory_order_relaxed);
            recursion_count_ = 1;
            return;
        }

        futex_wait(&futex_state_, kContended);
        spins = 1000;
    }
}

bool FutexRecursiveMutex::try_lock() {
    pid_t self = get_tid();

    if (owner_.load(std::memory_order_relaxed) == self) {
        ++recursion_count_;
        return true;
    }

    int expected = kUnlocked;
    if (futex_state_.compare_exchange_strong(expected, kLocked,
            std::memory_order_acquire, std::memory_order_relaxed)) {
        owner_.store(self, std::memory_order_relaxed);
        recursion_count_ = 1;
        return true;
    }

    return false;
}

void FutexRecursiveMutex::unlock() {
    pid_t self = get_tid();

    if (owner_.load(std::memory_order_relaxed) != self) {
        throw std::logic_error("FutexRecursiveMutex::unlock: not owner");
    }

    if (--recursion_count_ > 0) {
        return;
    }

    owner_.store(0, std::memory_order_relaxed);

    int prev = futex_state_.exchange(kUnlocked, std::memory_order_release);
    if (prev == kContended) {
        futex_wake_one(&futex_state_);
    }
}

unsigned int FutexRecursiveMutex::recursion_depth() const noexcept {
    pid_t self = get_tid();
    if (owner_.load(std::memory_order_relaxed) == self) {
        return recursion_count_;
    }
    return 0;
}

}  // namespace pman
