#pragma once

#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <thread>

namespace pman {

class Barrier {
public:
    explicit Barrier(std::size_t count) : threshold_(count), count_(count), generation_(0) {
        if (count == 0) {
            throw std::invalid_argument("Barrier requires count > 0");
        }
    }

    void wait() {
        std::size_t gen = generation_.load(std::memory_order_acquire);
        if (count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            count_.store(threshold_, std::memory_order_release);
            generation_.fetch_add(1, std::memory_order_acq_rel);
        } else {
            while (generation_.load(std::memory_order_acquire) == gen) {
                std::this_thread::yield();
            }
        }
    }

private:
    const std::size_t threshold_;
    std::atomic<std::size_t> count_;
    std::atomic<std::size_t> generation_;
};

}  // namespace pman
