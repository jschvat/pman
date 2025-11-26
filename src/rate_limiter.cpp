#include "pman/rate_limiter.hpp"

#include <algorithm>
#include <stdexcept>

namespace pman {

TokenBucketRateLimiter::TokenBucketRateLimiter(Config config)
    : config_(config),
      lastRefill_(std::chrono::steady_clock::now()) {
    if (config_.tokensPerSecond <= 0) {
        throw std::invalid_argument("tokensPerSecond must be positive");
    }
    if (config_.burstSize <= 0) {
        throw std::invalid_argument("burstSize must be positive");
    }

    tokens_ = config_.initialTokens >= 0 ? config_.initialTokens : config_.burstSize;
}

bool TokenBucketRateLimiter::tryAcquire(double tokens) {
    if (tokens <= 0) {
        throw std::invalid_argument("tokens must be positive");
    }

    FutexLockGuard lock(mutex_);
    refill();

    if (tokens_ >= tokens) {
        tokens_ -= tokens;
        return true;
    }
    return false;
}

void TokenBucketRateLimiter::acquire(double tokens) {
    if (tokens <= 0) {
        throw std::invalid_argument("tokens must be positive");
    }

    FutexLockGuard lock(mutex_);

    while (true) {
        refill();

        if (tokens_ >= tokens) {
            tokens_ -= tokens;
            return;
        }

        double needed = tokens - tokens_;
        auto waitTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(needed / config_.tokensPerSecond));

        cv_.wait_for(mutex_, waitTime);
    }
}

double TokenBucketRateLimiter::availableTokens() const {
    FutexLockGuard lock(const_cast<FutexMutex&>(mutex_));

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - lastRefill_).count();
    double refilled = tokens_ + elapsed * config_.tokensPerSecond;

    return std::min(refilled, config_.burstSize);
}

std::chrono::nanoseconds TokenBucketRateLimiter::timeUntilAvailable(double tokens) const {
    FutexLockGuard lock(const_cast<FutexMutex&>(mutex_));

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - lastRefill_).count();
    double current = std::min(tokens_ + elapsed * config_.tokensPerSecond, config_.burstSize);

    if (current >= tokens) {
        return std::chrono::nanoseconds::zero();
    }

    double needed = tokens - current;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(needed / config_.tokensPerSecond));
}

void TokenBucketRateLimiter::refill() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - lastRefill_).count();

    tokens_ = std::min(tokens_ + elapsed * config_.tokensPerSecond, config_.burstSize);
    lastRefill_ = now;
}

SlidingWindowRateLimiter::SlidingWindowRateLimiter(Config config)
    : config_(config) {
    if (config_.maxRequests == 0) {
        throw std::invalid_argument("maxRequests must be positive");
    }
    if (config_.windowDuration <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("windowDuration must be positive");
    }
}

bool SlidingWindowRateLimiter::tryAcquire() {
    FutexLockGuard lock(mutex_);
    cleanup();

    if (timestamps_.size() < config_.maxRequests) {
        timestamps_.push_back(std::chrono::steady_clock::now());
        return true;
    }
    return false;
}

void SlidingWindowRateLimiter::acquire() {
    FutexLockGuard lock(mutex_);

    while (true) {
        cleanup();

        if (timestamps_.size() < config_.maxRequests) {
            timestamps_.push_back(std::chrono::steady_clock::now());
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto oldestExpiry = timestamps_.front() + config_.windowDuration;
        auto waitTime = oldestExpiry > now ? oldestExpiry - now : std::chrono::nanoseconds::zero();

        cv_.wait_for(mutex_, waitTime);
    }
}

std::size_t SlidingWindowRateLimiter::remainingRequests() const {
    FutexLockGuard lock(const_cast<FutexMutex&>(mutex_));

    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - config_.windowDuration;

    std::size_t valid = 0;
    for (const auto& ts : timestamps_) {
        if (ts >= cutoff) {
            valid++;
        }
    }

    return config_.maxRequests > valid ? config_.maxRequests - valid : 0;
}

std::chrono::nanoseconds SlidingWindowRateLimiter::timeUntilReset() const {
    FutexLockGuard lock(const_cast<FutexMutex&>(mutex_));

    if (timestamps_.empty()) {
        return std::chrono::nanoseconds::zero();
    }

    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - config_.windowDuration;

    for (const auto& ts : timestamps_) {
        if (ts >= cutoff) {
            return (ts + config_.windowDuration) - now;
        }
    }

    return std::chrono::nanoseconds::zero();
}

void SlidingWindowRateLimiter::cleanup() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - config_.windowDuration;

    while (!timestamps_.empty() && timestamps_.front() < cutoff) {
        timestamps_.pop_front();
    }
}

}  // namespace pman
