#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "pman/thread.hpp"
#include "pman/topology.hpp"

namespace pman {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t workerCount, ThreadAttributes workerAttributes = {});
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> job);

    template <typename Callable>
    void submitTask(Callable&& callable) {
        submit(std::function<void()>(std::forward<Callable>(callable)));
    }

    void setAffinity(const CpuSet& set);
    void setScheduling(SchedulingPolicy policy, int priority);
    void pinWorkersToNode(int nodeId);
    void distributeByTopology(const SystemTopology& topology);

    [[nodiscard]] std::size_t workerCount() const noexcept { return workers_.size(); }

private:
    void workerLoop(std::size_t index);

    std::vector<ManagedThread> workers_;
    std::deque<std::function<void()>> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool shuttingDown_{false};
    ThreadAttributes workerAttributes_{};
};

}  // namespace pman
