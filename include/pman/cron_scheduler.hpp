#pragma once

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pman/thread.hpp"

namespace pman {

class CronExpression {
public:
    CronExpression();

    static CronExpression parse(std::string_view expression);

    static CronExpression everyMinute();
    static CronExpression everyHour();
    static CronExpression daily(int hour, int minute);
    static CronExpression weekly(int dayOfWeek, int hour, int minute);
    static CronExpression monthly(int dayOfMonth, int hour, int minute);

    CronExpression& minute(int value);
    CronExpression& hour(int value);
    CronExpression& dayOfMonth(int value);
    CronExpression& month(int value);
    CronExpression& dayOfWeek(int value);

    CronExpression& minuteRange(int start, int end);
    CronExpression& hourRange(int start, int end);

    [[nodiscard]] std::chrono::system_clock::time_point
    nextAfter(std::chrono::system_clock::time_point after) const;

    [[nodiscard]] std::string toString() const;

private:
    void setAll();

    std::bitset<60> minutes_;
    std::bitset<24> hours_;
    std::bitset<32> daysOfMonth_;
    std::bitset<13> months_;
    std::bitset<7> daysOfWeek_;
};

class CronScheduler {
public:
    CronScheduler();
    ~CronScheduler();

    CronScheduler(const CronScheduler&) = delete;
    CronScheduler& operator=(const CronScheduler&) = delete;

    std::uint64_t schedule(const std::string& name, CronExpression expr,
                           std::function<void()> task);

    bool cancel(std::uint64_t taskId);

    void start();
    void stop();

    [[nodiscard]] std::chrono::system_clock::time_point
    nextExecution(std::uint64_t taskId) const;

    struct TaskInfo {
        std::uint64_t id;
        std::string name;
        std::chrono::system_clock::time_point nextRun;
        std::uint64_t executionCount;
    };

    [[nodiscard]] std::vector<TaskInfo> listTasks() const;

private:
    void schedulerLoop();
    void scheduleWakeup();

    struct CronTask {
        std::uint64_t id;
        std::string name;
        CronExpression expression;
        std::function<void()> task;
        std::chrono::system_clock::time_point nextRun;
        std::uint64_t executionCount{0};
        bool active{true};
    };

    std::unordered_map<std::uint64_t, CronTask> tasks_;
    std::unique_ptr<ManagedThread> schedulerThread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> nextTaskId_{1};
    int timerFd_{-1};
    mutable std::mutex mutex_;
};

}  // namespace pman
