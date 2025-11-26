#pragma once

#include "event_loop_impl.hpp"
#include <linux/io_uring.h>
#include <linux/time_types.h>
#include <chrono>
#include <queue>
#include <map>
#include <functional>
#include <vector>
#include <memory>

namespace pman::async {

class IoUringLoop : public EventLoopImpl {
public:
    IoUringLoop(const EventLoopConfig& config);
    ~IoUringLoop() override;

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

    struct IoUringState {
        int ring_fd{-1};
        void* sq_ptr{nullptr};
        void* cq_ptr{nullptr};
        size_t sq_size{0};
        size_t cq_size{0};

        uint32_t* sq_head{nullptr};
        uint32_t* sq_tail{nullptr};
        uint32_t* sq_mask{nullptr};
        uint32_t* sq_entries{nullptr};
        uint32_t* sq_flags{nullptr};
        uint32_t* sq_dropped{nullptr};
        uint32_t* sq_array{nullptr};

        io_uring_sqe* sqes{nullptr};

        uint32_t* cq_head{nullptr};
        uint32_t* cq_tail{nullptr};
        uint32_t* cq_mask{nullptr};
        uint32_t* cq_entries{nullptr};
        io_uring_cqe* cqes{nullptr};
    };

    void processTimers();
    void processPostedCallbacks();
    void submitTimerOp();
    io_uring_sqe* getSqe();
    void submitSqes();
    void processCqes();

    IoUringState ring_;
    int eventFd_{-1};
    bool running_{false};

    std::priority_queue<TimerData, std::vector<TimerData>, std::greater<TimerData>> timers_;
    std::map<uint64_t, std::function<void()>> timerCallbacks_;
    std::map<uint64_t, std::chrono::nanoseconds> timerPeriods_;
    uint64_t nextTimerId_{1};

    std::map<uint64_t, std::function<void(int)>> userDataCallbacks_;
    uint64_t nextUserData_{1000};

    bool timerOpPending_{false};
    __kernel_timespec currentTimeout_{};
    std::vector<std::function<void()>> postedCallbacks_;
};

} // namespace pman::async
