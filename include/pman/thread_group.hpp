#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "pman/thread.hpp"

namespace pman {

class ThreadGroup {
public:
    ThreadGroup() = default;

    ManagedThread& add(std::string name, std::function<void()> task, ThreadAttributes attributes = {});

    template <typename Callable>
    ManagedThread& emplace(std::string name, Callable&& callable, ThreadAttributes attributes = {}) {
        return add(std::move(name), std::function<void()>(std::forward<Callable>(callable)), attributes);
    }

    void joinAll();
    void detachAll();
    void setAffinityAll(const CpuSet& set);
    void setSchedulingAll(SchedulingPolicy policy, int priority);

    [[nodiscard]] std::size_t size() const noexcept { return threads_.size(); }

private:
    std::vector<ManagedThread> threads_;
};

}  // namespace pman
