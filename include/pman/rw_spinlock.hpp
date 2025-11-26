#pragma once

#include <atomic>
#include <thread>

namespace pman {

class RwSpinLock {
public:
    void lock_shared() {
        while (true) {
            auto expected = state_.load(std::memory_order_relaxed);
            if (expected >= 0 && state_.compare_exchange_weak(expected, expected + 1, std::memory_order_acquire)) {
                return;
            }
            std::this_thread::yield();
        }
    }

    void unlock_shared() {
        state_.fetch_sub(1, std::memory_order_release);
    }

    void lock() {
        while (true) {
            int expected = 0;
            if (state_.compare_exchange_weak(expected, -1, std::memory_order_acquire)) {
                return;
            }
            std::this_thread::yield();
        }
    }

    void unlock() {
        state_.store(0, std::memory_order_release);
    }

private:
    std::atomic<int> state_{0};
};

}  // namespace pman
