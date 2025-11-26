#include "pman/periodic_scheduler.hpp"

#include <stdexcept>
#include <utility>

namespace pman {

PeriodicScheduler::PeriodicScheduler(std::size_t workerCount) {
    TimerWheel::Config timerConfig;
    timerConfig.tickDuration = std::chrono::milliseconds(1);
    timerConfig.wheelSize = 256;
    timerConfig.numWheels = 4;

    timerWheel_ = std::make_unique<TimerWheel>(timerConfig);
    workers_ = std::make_unique<EnhancedThreadPool>(workerCount);
}

PeriodicScheduler::~PeriodicScheduler() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

std::uint64_t PeriodicScheduler::schedule(PeriodicTaskConfig config, std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("Task cannot be null");
    }
    if (config.period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("Period must be positive");
    }

    std::uint64_t taskId = nextTaskId_.fetch_add(1, std::memory_order_relaxed);

    auto state = std::make_shared<TaskState>();
    state->config = std::move(config);
    state->task = std::move(task);
    state->stopSource = std::make_shared<std::stop_source>();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[taskId] = state;
    }

    if (running_.load(std::memory_order_acquire)) {
        scheduleNext(taskId, true);
    }

    return taskId;
}

std::uint64_t PeriodicScheduler::schedule(PeriodicTaskConfig config,
                                           std::function<void(std::stop_token)> task) {
    if (!task) {
        throw std::invalid_argument("Task cannot be null");
    }

    std::uint64_t taskId = nextTaskId_.fetch_add(1, std::memory_order_relaxed);

    auto state = std::make_shared<TaskState>();
    state->config = std::move(config);
    state->stopSource = std::make_shared<std::stop_source>();

    auto stopSource = state->stopSource;
    state->task = [task = std::move(task), stopSource]() {
        task(stopSource->get_token());
    };

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[taskId] = state;
    }

    if (running_.load(std::memory_order_acquire)) {
        scheduleNext(taskId, true);
    }

    return taskId;
}

bool PeriodicScheduler::cancel(std::uint64_t taskId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) {
        return false;
    }

    auto& state = it->second;
    state->active = false;
    state->stopSource->request_stop();

    if (state->timerId != 0) {
        timerWheel_->cancel(state->timerId);
    }

    return true;
}

void PeriodicScheduler::pause(std::uint64_t taskId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(taskId);
    if (it != tasks_.end()) {
        it->second->paused = true;
    }
}

void PeriodicScheduler::resume(std::uint64_t taskId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(taskId);
    if (it != tasks_.end() && it->second->paused) {
        it->second->paused = false;
        if (running_.load(std::memory_order_acquire)) {
            scheduleNext(taskId, false);
        }
    }
}

void PeriodicScheduler::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(true, std::memory_order_release);
    timerWheel_->start();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [taskId, state] : tasks_) {
        if (state->active && !state->paused) {
            scheduleNext(taskId, true);
        }
    }
}

void PeriodicScheduler::stop() {
    running_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [taskId, state] : tasks_) {
            state->stopSource->request_stop();
            if (state->timerId != 0) {
                timerWheel_->cancel(state->timerId);
            }
        }
    }

    timerWheel_->stop();
}

void PeriodicScheduler::stopGracefully(std::chrono::nanoseconds timeout) {
    running_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [taskId, state] : tasks_) {
            state->stopSource->request_stop();
            if (state->timerId != 0) {
                timerWheel_->cancel(state->timerId);
            }
        }
    }

    workers_->waitIdleFor(timeout);
    timerWheel_->stop();
}

std::vector<PeriodicScheduler::TaskStatus> PeriodicScheduler::status() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TaskStatus> result;
    result.reserve(tasks_.size());

    for (const auto& [taskId, state] : tasks_) {
        TaskStatus s;
        s.id = taskId;
        s.name = state->config.name;
        s.active = state->active;
        s.paused = state->paused;
        s.executionCount = state->executionCount;
        s.lastExecution = state->lastExecution;
        s.nextScheduled = state->nextScheduled;
        s.averageExecutionTime = state->executionCount > 0
            ? std::chrono::nanoseconds(state->totalExecutionTime / state->executionCount)
            : std::chrono::nanoseconds::zero();
        result.push_back(s);
    }

    return result;
}

PeriodicScheduler::TaskStatus PeriodicScheduler::status(std::uint64_t taskId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) {
        throw std::runtime_error("Unknown task ID");
    }

    const auto& state = it->second;
    TaskStatus s;
    s.id = taskId;
    s.name = state->config.name;
    s.active = state->active;
    s.paused = state->paused;
    s.executionCount = state->executionCount;
    s.lastExecution = state->lastExecution;
    s.nextScheduled = state->nextScheduled;
    s.averageExecutionTime = state->executionCount > 0
        ? std::chrono::nanoseconds(state->totalExecutionTime / state->executionCount)
        : std::chrono::nanoseconds::zero();

    return s;
}

void PeriodicScheduler::scheduleNext(std::uint64_t taskId, bool isFirst) {
    std::shared_ptr<TaskState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) {
            return;
        }
        state = it->second;
    }

    if (!state->active || state->paused) {
        return;
    }

    std::chrono::nanoseconds delay;
    if (isFirst) {
        delay = state->config.initialDelay;
    } else if (state->config.fixedRate) {
        auto now = std::chrono::steady_clock::now();
        auto nextTarget = state->nextScheduled + state->config.period;
        delay = nextTarget > now ? nextTarget - now : std::chrono::nanoseconds::zero();
    } else {
        delay = state->config.period;
    }

    state->nextScheduled = std::chrono::steady_clock::now() + delay;

    state->timerId = timerWheel_->schedule(delay, [this, taskId]() {
        executeTask(taskId);
    });
}

void PeriodicScheduler::executeTask(std::uint64_t taskId) {
    std::shared_ptr<TaskState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) {
            return;
        }
        state = it->second;
    }

    if (!state->active || state->paused) {
        return;
    }

    if (state->runningCount.load(std::memory_order_acquire) >= state->config.maxConcurrent) {
        if (!state->config.catchUp) {
            scheduleNext(taskId, false);
            return;
        }
    }

    state->runningCount.fetch_add(1, std::memory_order_relaxed);

    workers_->submit([this, taskId, state]() {
        auto startTime = std::chrono::steady_clock::now();

        try {
            state->task();
        } catch (...) {
        }

        auto endTime = std::chrono::steady_clock::now();
        auto execTime = endTime - startTime;

        state->runningCount.fetch_sub(1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            state->executionCount++;
            state->lastExecution = startTime;
            state->totalExecutionTime += execTime;
        }

        if (running_.load(std::memory_order_acquire) && state->active && !state->paused) {
            scheduleNext(taskId, false);
        }
    });
}

}  // namespace pman
