#include "pman/thread_pool.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace pman {

ThreadPool::ThreadPool(std::size_t workerCount, ThreadAttributes workerAttributes)
    : workerAttributes_(std::move(workerAttributes)) {
    if (workerCount == 0) {
        throw std::invalid_argument("ThreadPool requires at least one worker");
    }

    workers_.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        std::string name = "pman-worker-" + std::to_string(i);
        workers_.emplace_back(
            name,
            [this, i]() { workerLoop(i); },
            workerAttributes_);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::submit(std::function<void()> job) {
    if (!job) {
        throw std::invalid_argument("ThreadPool::submit requires a callable");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) {
            throw std::runtime_error("ThreadPool is shutting down");
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void ThreadPool::setAffinity(const CpuSet& set) {
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.setCpuAffinity(set);
        }
    }
}

void ThreadPool::setScheduling(SchedulingPolicy policy, int priority) {
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.setScheduling(policy, priority);
        }
    }
}

void ThreadPool::pinWorkersToNode(int nodeId) {
    CpuSet set = SystemTopology::instance().cpuSetForNode(nodeId);
    setAffinity(set);
}

void ThreadPool::distributeByTopology(const SystemTopology& topology) {
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

void ThreadPool::workerLoop(std::size_t /*index*/) {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return shuttingDown_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (shuttingDown_) {
                    break;
                }
                continue;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            job();
        } catch (...) {
            // Swallow exceptions to keep the pool alive.
        }
    }
}

}  // namespace pman
