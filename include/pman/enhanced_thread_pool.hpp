#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "pman/thread.hpp"
#include "pman/topology.hpp"

namespace pman {

struct TaskHandle {
    std::uint64_t id{0};
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::shared_ptr<std::atomic<bool>> completed;

    bool cancel();
    [[nodiscard]] bool isCancelled() const noexcept;
    [[nodiscard]] bool isComplete() const noexcept;
};

class CancellationToken;

struct ThreadPoolMetrics {
    std::uint64_t tasksSubmitted{0};
    std::uint64_t tasksCompleted{0};
    std::uint64_t tasksCancelled{0};
    std::uint64_t tasksFailed{0};
    std::size_t queueDepth{0};
    std::size_t activeWorkers{0};
    std::chrono::nanoseconds totalWaitTime{0};
    std::chrono::nanoseconds totalExecutionTime{0};
    std::chrono::steady_clock::time_point lastActivity;
};

class EnhancedThreadPool {
public:
    explicit EnhancedThreadPool(std::size_t workerCount, ThreadAttributes attributes = {});
    ~EnhancedThreadPool();

    EnhancedThreadPool(const EnhancedThreadPool&) = delete;
    EnhancedThreadPool& operator=(const EnhancedThreadPool&) = delete;

    TaskHandle submit(std::function<void()> job);
    TaskHandle submit(std::function<void(CancellationToken&)> job);

    TaskHandle submitWithPriority(int priority, std::function<void()> job);
    TaskHandle submitWithPriority(int priority, std::function<void(CancellationToken&)> job);

    void cancelAll();
    void waitIdle();
    bool waitIdleFor(std::chrono::nanoseconds timeout);

    [[nodiscard]] ThreadPoolMetrics metrics() const;
    void resetMetrics();

    void resize(std::size_t newWorkerCount);

    void setAffinity(const CpuSet& set);
    void setScheduling(SchedulingPolicy policy, int priority);
    void pinWorkersToNode(int nodeId);
    void distributeByTopology(const SystemTopology& topology);

    [[nodiscard]] std::size_t workerCount() const noexcept;
    [[nodiscard]] std::size_t queueSize() const;
    [[nodiscard]] std::size_t activeWorkerCount() const noexcept;

private:
    struct TaskEntry {
        std::uint64_t id;
        int priority;
        std::uint64_t sequenceNumber;
        std::function<void()> task;
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::shared_ptr<std::atomic<bool>> completed;
        std::chrono::steady_clock::time_point submitTime;

        bool operator<(const TaskEntry& other) const {
            if (priority != other.priority) {
                return priority > other.priority;
            }
            return sequenceNumber > other.sequenceNumber;
        }
    };

    void workerLoop(std::size_t index);
    TaskHandle createTaskHandle();
    void updateMetricsOnComplete(bool success, std::chrono::nanoseconds waitTime,
                                 std::chrono::nanoseconds execTime);

    std::vector<ManagedThread> workers_;
    std::priority_queue<TaskEntry> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idleCv_;
    std::atomic<bool> shuttingDown_{false};
    std::atomic<std::size_t> activeWorkers_{0};
    std::atomic<std::uint64_t> nextTaskId_{1};
    std::atomic<std::uint64_t> sequenceCounter_{0};
    ThreadAttributes workerAttributes_;

    mutable std::mutex metricsMutex_;
    ThreadPoolMetrics metrics_;
};

class CancellationToken {
public:
    [[nodiscard]] bool isCancelled() const noexcept {
        return cancelled_ && cancelled_->load(std::memory_order_acquire);
    }

    void throwIfCancelled() const {
        if (isCancelled()) {
            throw std::runtime_error("Task was cancelled");
        }
    }

private:
    friend class EnhancedThreadPool;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> cancelled)
        : cancelled_(std::move(cancelled)) {}

    std::shared_ptr<std::atomic<bool>> cancelled_;
};

}  // namespace pman
