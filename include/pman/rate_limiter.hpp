#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>

#include "pman/futex.hpp"

namespace pman {

class TokenBucketRateLimiter {
public:
    struct Config {
        double tokensPerSecond;
        double burstSize;
        double initialTokens{-1};
    };

    explicit TokenBucketRateLimiter(Config config);

    bool tryAcquire(double tokens = 1.0);
    void acquire(double tokens = 1.0);

    template <typename Rep, typename Period>
    bool tryAcquireFor(double tokens, const std::chrono::duration<Rep, Period>& timeout);

    [[nodiscard]] double availableTokens() const;
    [[nodiscard]] std::chrono::nanoseconds timeUntilAvailable(double tokens) const;

private:
    void refill();

    Config config_;
    double tokens_;
    std::chrono::steady_clock::time_point lastRefill_;
    mutable FutexMutex mutex_;
    FutexCondVar cv_;
};

class SlidingWindowRateLimiter {
public:
    struct Config {
        std::size_t maxRequests;
        std::chrono::nanoseconds windowDuration;
    };

    explicit SlidingWindowRateLimiter(Config config);

    bool tryAcquire();
    void acquire();

    template <typename Rep, typename Period>
    bool tryAcquireFor(const std::chrono::duration<Rep, Period>& timeout);

    [[nodiscard]] std::size_t remainingRequests() const;
    [[nodiscard]] std::chrono::nanoseconds timeUntilReset() const;

private:
    void cleanup();

    Config config_;
    std::deque<std::chrono::steady_clock::time_point> timestamps_;
    mutable FutexMutex mutex_;
    FutexCondVar cv_;
};

template <typename Rep, typename Period>
bool TokenBucketRateLimiter::tryAcquireFor(double tokens,
                                            const std::chrono::duration<Rep, Period>& timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;

    FutexLockGuard lock(mutex_);

    while (true) {
        refill();

        if (tokens_ >= tokens) {
            tokens_ -= tokens;
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }

        auto waitTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>((tokens - tokens_) / config_.tokensPerSecond));

        auto maxWait = deadline - now;
        if (waitTime > maxWait) {
            waitTime = std::chrono::duration_cast<std::chrono::nanoseconds>(maxWait);
        }

        cv_.wait_for(mutex_, waitTime);
    }
}

template <typename Rep, typename Period>
bool SlidingWindowRateLimiter::tryAcquireFor(const std::chrono::duration<Rep, Period>& timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;

    FutexLockGuard lock(mutex_);

    while (true) {
        cleanup();

        if (timestamps_.size() < config_.maxRequests) {
            timestamps_.push_back(std::chrono::steady_clock::now());
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }

        auto oldestExpiry = timestamps_.front() + config_.windowDuration;
        auto waitTime = oldestExpiry > now ? oldestExpiry - now : std::chrono::nanoseconds::zero();

        auto maxWait = deadline - now;
        if (waitTime > maxWait) {
            waitTime = maxWait;
        }

        cv_.wait_for(mutex_, waitTime);
    }
}

}  // namespace pman
