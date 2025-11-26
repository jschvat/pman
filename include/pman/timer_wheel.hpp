#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace pman {

using TimerCallback = std::function<void()>;

class TimerWheel {
public:
    struct Config {
        std::chrono::nanoseconds tickDuration;
        std::size_t wheelSize;
        std::size_t numWheels;

        Config() : tickDuration(std::chrono::milliseconds(1)), wheelSize(256), numWheels(4) {}
    };

    explicit TimerWheel(Config config = Config());
    ~TimerWheel();

    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    std::uint64_t schedule(std::chrono::nanoseconds delay, TimerCallback callback);
    std::uint64_t schedulePeriodic(std::chrono::nanoseconds period, TimerCallback callback);

    bool cancel(std::uint64_t timerId);

    void tick();
    void advance(std::chrono::nanoseconds elapsed);
    std::size_t processExpired();

    [[nodiscard]] std::chrono::nanoseconds timeUntilNextExpiry() const;
    [[nodiscard]] int timerFd() const noexcept { return timerFd_; }

    [[nodiscard]] std::size_t activeTimers() const noexcept;
    [[nodiscard]] std::size_t pendingExpirations() const noexcept;

    void start();
    void stop();

private:
    struct TimerEntry {
        std::uint64_t id;
        std::chrono::steady_clock::time_point expiry;
        std::chrono::nanoseconds period{0};
        TimerCallback callback;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    void insert(TimerEntry entry);
    void cascade(std::size_t wheelIndex);
    std::size_t computeSlot(std::chrono::nanoseconds delay, std::size_t wheelIndex) const;
    void armTimerFd();

    Config config_;
    std::vector<std::vector<std::deque<TimerEntry>>> wheels_;
    std::vector<std::size_t> currentSlots_;
    std::chrono::steady_clock::time_point lastTick_;
    std::atomic<std::uint64_t> nextId_{1};
    std::atomic<std::size_t> activeCount_{0};
    int timerFd_{-1};
    mutable std::mutex mutex_;

    std::unique_ptr<class ManagedThread> timerThread_;
    std::atomic<bool> running_{false};
};

}  // namespace pman
