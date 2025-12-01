#pragma once

#include "event_loop_impl.hpp"
#include <sys/epoll.h>
#include <chrono>
#include <map>
#include <unordered_map>
#include <functional>
#include <vector>
#include <optional>

namespace pman::async {

class EpollLoop : public EventLoopImpl {
public:
    EpollLoop(const EventLoopConfig& config);
    ~EpollLoop() override;

    void run() override;
    bool runOnce(std::chrono::nanoseconds timeout) override;
    void stop() override;
    bool isRunning() const noexcept override;
    BackendType backend() const noexcept override;

    void addFd(int fd, Event events, EventCallback callback) override;
    void modifyFd(int fd, Event events) override;
    void removeFd(int fd) override;

    uint64_t addTimer(std::chrono::nanoseconds duration, std::function<void()> callback) override;
    uint64_t addPeriodicTimer(std::chrono::nanoseconds period, std::function<void()> callback) override;
    bool cancelTimer(uint64_t timerId) override;

    void post(std::function<void()> callback) override;
    size_t activeFds() const noexcept override;
    size_t activeTimers() const noexcept override;

private:
    // PHASE 2 OPTIMIZATION: Consolidated timer entry - all data in one place
    struct TimerEntry {
        uint64_t id;
        std::function<void()> callback;
        std::chrono::nanoseconds period{0};  // 0 = one-shot, >0 = periodic
    };

    void processTimers();
    void processPostedCallbacks();
    std::chrono::milliseconds getNextTimerTimeout() const;

    int epollFd_{-1};
    int eventFd_{-1};
    bool running_{false};

    // PHASE 2 OPTIMIZATION: Use multimap instead of priority_queue
    // - Sorted by expiry time (earliest first)
    // - O(1) access to next timer
    // - Efficient iteration for batch processing
    // - Easy cancellation via id lookup
    using TimePoint = std::chrono::steady_clock::time_point;
    std::multimap<TimePoint, TimerEntry> timersByExpiry_;

    // PHASE 2 OPTIMIZATION: Fast lookup for cancellation - maps id to iterator
    std::unordered_map<uint64_t, std::multimap<TimePoint, TimerEntry>::iterator> timerById_;

    uint64_t nextTimerId_{1};

    std::map<int, EventCallback> fdCallbacks_;
    std::vector<std::function<void()>> postedCallbacks_;
};

} // namespace pman::async
