#pragma once

#include "event_loop_impl.hpp"
#include <sys/epoll.h>
#include <chrono>
#include <queue>
#include <map>
#include <functional>
#include <vector>

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
    struct TimerData {
        uint64_t id;
        std::chrono::steady_clock::time_point expiry;

        bool operator>(const TimerData& other) const {
            return expiry > other.expiry;
        }
    };

    void processTimers();
    void processPostedCallbacks();
    std::chrono::milliseconds getNextTimerTimeout() const;

    int epollFd_{-1};
    int eventFd_{-1};
    bool running_{false};

    std::priority_queue<TimerData, std::vector<TimerData>, std::greater<TimerData>> timers_;
    std::map<uint64_t, std::function<void()>> timerCallbacks_;
    std::map<uint64_t, std::chrono::nanoseconds> timerPeriods_;
    uint64_t nextTimerId_{1};

    std::map<int, EventCallback> fdCallbacks_;
    std::vector<std::function<void()>> postedCallbacks_;
};

} // namespace pman::async
