#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <vector>

#include "pman/enhanced_thread_pool.hpp"
#include "pman/timer_wheel.hpp"

namespace pman {

struct PeriodicTaskConfig {
    std::string name;
    std::chrono::nanoseconds period;
    std::chrono::nanoseconds initialDelay{0};
    bool fixedRate{false};
    bool catchUp{false};
    int maxConcurrent{1};
};

class PeriodicScheduler {
public:
    explicit PeriodicScheduler(std::size_t workerCount = 1);
    ~PeriodicScheduler();

    PeriodicScheduler(const PeriodicScheduler&) = delete;
    PeriodicScheduler& operator=(const PeriodicScheduler&) = delete;

    std::uint64_t schedule(PeriodicTaskConfig config, std::function<void()> task);
    std::uint64_t schedule(PeriodicTaskConfig config, std::function<void(std::stop_token)> task);

    bool cancel(std::uint64_t taskId);
    void pause(std::uint64_t taskId);
    void resume(std::uint64_t taskId);

    void start();
    void stop();
    void stopGracefully(std::chrono::nanoseconds timeout);

    struct TaskStatus {
        std::uint64_t id;
        std::string name;
        bool active;
        bool paused;
        std::uint64_t executionCount;
        std::chrono::steady_clock::time_point lastExecution;
        std::chrono::steady_clock::time_point nextScheduled;
        std::chrono::nanoseconds averageExecutionTime;
    };

    [[nodiscard]] std::vector<TaskStatus> status() const;
    [[nodiscard]] TaskStatus status(std::uint64_t taskId) const;

private:
    struct TaskState {
        PeriodicTaskConfig config;
        std::function<void()> task;
        std::shared_ptr<std::stop_source> stopSource;
        std::uint64_t timerId{0};
        bool active{true};
        bool paused{false};
        std::atomic<int> runningCount{0};
        std::uint64_t executionCount{0};
        std::chrono::steady_clock::time_point lastExecution;
        std::chrono::steady_clock::time_point nextScheduled;
        std::chrono::nanoseconds totalExecutionTime{0};
    };

    void scheduleNext(std::uint64_t taskId, bool isFirst);
    void executeTask(std::uint64_t taskId);

    std::unique_ptr<TimerWheel> timerWheel_;
    std::unique_ptr<EnhancedThreadPool> workers_;
    std::unordered_map<std::uint64_t, std::shared_ptr<TaskState>> tasks_;
    std::atomic<std::uint64_t> nextTaskId_{1};
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
};

}  // namespace pman
