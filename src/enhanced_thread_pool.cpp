#include "pman/enhanced_thread_pool.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace pman {

bool TaskHandle::cancel() {
    if (cancelled && !completed->load(std::memory_order_acquire)) {
        cancelled->store(true, std::memory_order_release);
        return true;
    }
    return false;
}

bool TaskHandle::isCancelled() const noexcept {
    return cancelled && cancelled->load(std::memory_order_acquire);
}

bool TaskHandle::isComplete() const noexcept {
    return completed && completed->load(std::memory_order_acquire);
}

EnhancedThreadPool::EnhancedThreadPool(std::size_t workerCount, ThreadAttributes attributes)
    : workerAttributes_(std::move(attributes)) {
    if (workerCount == 0) {
        throw std::invalid_argument("EnhancedThreadPool requires at least one worker");
    }

    workers_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        std::string name = "pman-epool-" + std::to_string(i);
        workers_.emplace_back(
            name,
            [this, i]() { workerLoop(i); },
            workerAttributes_);
    }
}

EnhancedThreadPool::~EnhancedThreadPool() {
    shuttingDown_.store(true, std::memory_order_release);
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            try {
                worker.join();
            } catch (...) {}
        }
    }
}

TaskHandle EnhancedThreadPool::createTaskHandle() {
    TaskHandle handle;
    handle.id = nextTaskId_.fetch_add(1, std::memory_order_relaxed);
    handle.cancelled = std::make_shared<std::atomic<bool>>(false);
    handle.completed = std::make_shared<std::atomic<bool>>(false);
    return handle;
}

TaskHandle EnhancedThreadPool::submit(std::function<void()> job) {
    return submitWithPriority(0, std::move(job));
}

TaskHandle EnhancedThreadPool::submit(std::function<void(CancellationToken&)> job) {
    return submitWithPriority(0, std::move(job));
}

TaskHandle EnhancedThreadPool::submitWithPriority(int priority, std::function<void()> job) {
    if (!job) {
        throw std::invalid_argument("EnhancedThreadPool::submit requires a callable");
    }

    TaskHandle handle = createTaskHandle();

    TaskEntry entry;
    entry.id = handle.id;
    entry.priority = priority;
    entry.sequenceNumber = sequenceCounter_.fetch_add(1, std::memory_order_relaxed);
    entry.task = std::move(job);
    entry.cancelled = handle.cancelled;
    entry.completed = handle.completed;
    entry.submitTime = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_.load(std::memory_order_acquire)) {
            throw std::runtime_error("EnhancedThreadPool is shutting down");
        }
        queue_.push(std::move(entry));
    }

    {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        ++metrics_.tasksSubmitted;
        metrics_.queueDepth = queue_.size();
    }

    cv_.notify_one();
    return handle;
}

TaskHandle EnhancedThreadPool::submitWithPriority(int priority,
                                                   std::function<void(CancellationToken&)> job) {
    if (!job) {
        throw std::invalid_argument("EnhancedThreadPool::submit requires a callable");
    }

    TaskHandle handle = createTaskHandle();
    auto cancelled = handle.cancelled;

    auto wrapper = [job = std::move(job), cancelled]() {
        CancellationToken token(cancelled);
        job(token);
    };

    TaskEntry entry;
    entry.id = handle.id;
    entry.priority = priority;
    entry.sequenceNumber = sequenceCounter_.fetch_add(1, std::memory_order_relaxed);
    entry.task = std::move(wrapper);
    entry.cancelled = handle.cancelled;
    entry.completed = handle.completed;
    entry.submitTime = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_.load(std::memory_order_acquire)) {
            throw std::runtime_error("EnhancedThreadPool is shutting down");
        }
        queue_.push(std::move(entry));
    }

    {
        std::lock_guard<std::mutex> lock(metricsMutex_);
        ++metrics_.tasksSubmitted;
        metrics_.queueDepth = queue_.size();
    }

    cv_.notify_one();
    return handle;
}

void EnhancedThreadPool::cancelAll() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::priority_queue<TaskEntry> newQueue;
    while (!queue_.empty()) {
        TaskEntry entry = std::move(const_cast<TaskEntry&>(queue_.top()));
        queue_.pop();
        entry.cancelled->store(true, std::memory_order_release);

        {
            std::lock_guard<std::mutex> mlock(metricsMutex_);
            ++metrics_.tasksCancelled;
        }
    }
    queue_ = std::move(newQueue);

    {
        std::lock_guard<std::mutex> mlock(metricsMutex_);
        metrics_.queueDepth = 0;
    }
}

void EnhancedThreadPool::waitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idleCv_.wait(lock, [this]() {
        return queue_.empty() && activeWorkers_.load(std::memory_order_acquire) == 0;
    });
}

bool EnhancedThreadPool::waitIdleFor(std::chrono::nanoseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return idleCv_.wait_for(lock, timeout, [this]() {
        return queue_.empty() && activeWorkers_.load(std::memory_order_acquire) == 0;
    });
}

ThreadPoolMetrics EnhancedThreadPool::metrics() const {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    ThreadPoolMetrics m = metrics_;
    m.activeWorkers = activeWorkers_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> qlock(mutex_);
        m.queueDepth = queue_.size();
    }
    return m;
}

void EnhancedThreadPool::resetMetrics() {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    metrics_ = ThreadPoolMetrics{};
}

void EnhancedThreadPool::resize(std::size_t newWorkerCount) {
    if (newWorkerCount == 0) {
        throw std::invalid_argument("EnhancedThreadPool requires at least one worker");
    }

    std::size_t currentCount = workers_.size();

    if (newWorkerCount > currentCount) {
        for (std::size_t i = currentCount; i < newWorkerCount; ++i) {
            std::string name = "pman-epool-" + std::to_string(i);
            workers_.emplace_back(
                name,
                [this, i]() { workerLoop(i); },
                workerAttributes_);
        }
    } else if (newWorkerCount < currentCount) {
        // Cannot shrink dynamically without complex logic
        throw std::runtime_error("Shrinking pool not yet supported");
    }
}

void EnhancedThreadPool::setAffinity(const CpuSet& set) {
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.setCpuAffinity(set);
        }
    }
}

void EnhancedThreadPool::setScheduling(SchedulingPolicy policy, int priority) {
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.setScheduling(policy, priority);
        }
    }
}

void EnhancedThreadPool::pinWorkersToNode(int nodeId) {
    CpuSet set = SystemTopology::instance().cpuSetForNode(nodeId);
    setAffinity(set);
}

void EnhancedThreadPool::distributeByTopology(const SystemTopology& topology) {
    auto cpus = topology.cpuIds();
    if (cpus.empty()) {
        return;
    }
    for (std::size_t i = 0; i < workers_.size(); ++i) {
        CpuSet set;
        set.add(cpus[i % cpus.size()]);
        if (workers_[i].joinable()) {
            workers_[i].setCpuAffinity(set);
        }
    }
}

std::size_t EnhancedThreadPool::workerCount() const noexcept {
    return workers_.size();
}

std::size_t EnhancedThreadPool::queueSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t EnhancedThreadPool::activeWorkerCount() const noexcept {
    return activeWorkers_.load(std::memory_order_acquire);
}

void EnhancedThreadPool::updateMetricsOnComplete(bool success,
                                                  std::chrono::nanoseconds waitTime,
                                                  std::chrono::nanoseconds execTime) {
    std::lock_guard<std::mutex> lock(metricsMutex_);
    if (success) {
        ++metrics_.tasksCompleted;
    } else {
        ++metrics_.tasksFailed;
    }
    metrics_.totalWaitTime += waitTime;
    metrics_.totalExecutionTime += execTime;
    metrics_.lastActivity = std::chrono::steady_clock::now();
}

void EnhancedThreadPool::workerLoop(std::size_t /*index*/) {
    while (true) {
        TaskEntry entry;
        bool hasTask = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return shuttingDown_.load(std::memory_order_acquire) || !queue_.empty();
            });

            if (queue_.empty()) {
                if (shuttingDown_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            entry = std::move(const_cast<TaskEntry&>(queue_.top()));
            queue_.pop();
            hasTask = true;
        }

        if (!hasTask) {
            continue;
        }

        if (entry.cancelled->load(std::memory_order_acquire)) {
            entry.completed->store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(metricsMutex_);
                ++metrics_.tasksCancelled;
            }
            idleCv_.notify_all();
            continue;
        }

        activeWorkers_.fetch_add(1, std::memory_order_relaxed);

        auto startTime = std::chrono::steady_clock::now();
        auto waitTime = startTime - entry.submitTime;

        bool success = true;
        try {
            entry.task();
        } catch (...) {
            success = false;
        }

        auto endTime = std::chrono::steady_clock::now();
        auto execTime = endTime - startTime;

        entry.completed->store(true, std::memory_order_release);
        activeWorkers_.fetch_sub(1, std::memory_order_relaxed);

        updateMetricsOnComplete(success, waitTime, execTime);

        idleCv_.notify_all();
    }
}

}  // namespace pman
