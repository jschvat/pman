#pragma once

#include <chrono>
#include <cstdint>

namespace pman::async {

class Duration {
public:
    constexpr Duration() : nanos_(0) {}
    
    static constexpr Duration fromNanos(int64_t nanos) {
        Duration d;
        d.nanos_ = nanos;
        return d;
    }
    
    static constexpr Duration fromMicros(int64_t micros) {
        return fromNanos(micros * 1000);
    }
    
    static constexpr Duration fromMillis(int64_t millis) {
        return fromNanos(millis * 1000000);
    }
    
    static constexpr Duration fromSeconds(int64_t secs) {
        return fromNanos(secs * 1000000000);
    }
    
    constexpr int64_t asNanos() const { return nanos_; }
    constexpr int64_t asMicros() const { return nanos_ / 1000; }
    constexpr int64_t asMillis() const { return nanos_ / 1000000; }
    constexpr int64_t asSeconds() const { return nanos_ / 1000000000; }
    
    std::chrono::nanoseconds toChrono() const {
        return std::chrono::nanoseconds(nanos_);
    }
    
    constexpr bool operator==(const Duration& other) const {
        return nanos_ == other.nanos_;
    }
    
    constexpr bool operator!=(const Duration& other) const {
        return nanos_ != other.nanos_;
    }
    
    constexpr bool operator<(const Duration& other) const {
        return nanos_ < other.nanos_;
    }
    
    constexpr bool operator<=(const Duration& other) const {
        return nanos_ <= other.nanos_;
    }
    
    constexpr bool operator>(const Duration& other) const {
        return nanos_ > other.nanos_;
    }
    
    constexpr bool operator>=(const Duration& other) const {
        return nanos_ >= other.nanos_;
    }
    
    constexpr Duration operator+(const Duration& other) const {
        return Duration::fromNanos(nanos_ + other.nanos_);
    }
    
    constexpr Duration operator-(const Duration& other) const {
        return Duration::fromNanos(nanos_ - other.nanos_);
    }
    
private:
    int64_t nanos_;
};

class Instant {
public:
    static Instant now() {
        Instant i;
        i.tp_ = std::chrono::steady_clock::now();
        return i;
    }
    
    bool hasPassed() const {
        return std::chrono::steady_clock::now() >= tp_;
    }
    
    Duration durationUntil() const {
        auto now = std::chrono::steady_clock::now();
        if (now >= tp_) {
            return Duration::fromNanos(0);
        }
        auto diff = std::chrono::duration_cast<std::chrono::nanoseconds>(tp_ - now);
        return Duration::fromNanos(diff.count());
    }
    
    bool operator==(const Instant& other) const {
        return tp_ == other.tp_;
    }
    
    bool operator!=(const Instant& other) const {
        return tp_ != other.tp_;
    }
    
    bool operator<(const Instant& other) const {
        return tp_ < other.tp_;
    }
    
    bool operator<=(const Instant& other) const {
        return tp_ <= other.tp_;
    }
    
    bool operator>(const Instant& other) const {
        return tp_ > other.tp_;
    }
    
    bool operator>=(const Instant& other) const {
        return tp_ >= other.tp_;
    }
    
    Instant operator+(const Duration& d) const {
        Instant i;
        i.tp_ = tp_ + d.toChrono();
        return i;
    }
    
    Instant operator-(const Duration& d) const {
        Instant i;
        i.tp_ = tp_ - d.toChrono();
        return i;
    }
    
    Duration operator-(const Instant& other) const {
        auto diff = std::chrono::duration_cast<std::chrono::nanoseconds>(tp_ - other.tp_);
        return Duration::fromNanos(diff.count());
    }
    
private:
    std::chrono::steady_clock::time_point tp_;
};

} // namespace pman::async
