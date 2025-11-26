#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace pman {

class SeqLock {
public:
    void lock() {
        writerLock_.lock();
        seq_.fetch_add(1, std::memory_order_acq_rel);
    }

    void unlock() {
        seq_.fetch_add(1, std::memory_order_release);
        writerLock_.unlock();
    }

    template <typename Fn>
    auto read(Fn&& fn) const -> decltype(fn()) {
        while (true) {
            std::size_t version = seq_.load(std::memory_order_acquire);
            if (version & 1) {
                std::this_thread::yield();
                continue;
            }
            if constexpr (std::is_void_v<decltype(fn())>) {
                fn();
                if (seq_.load(std::memory_order_acquire) == version) {
                    return;
                }
            } else {
                auto result = fn();
                if (seq_.load(std::memory_order_acquire) == version) {
                    return result;
                }
            }
        }
    }

private:
    mutable std::atomic<std::size_t> seq_{0};
    mutable std::mutex writerLock_;
};

}  // namespace pman
