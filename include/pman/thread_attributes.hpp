#pragma once

#include <cstddef>
#include <optional>

#include "pman/cpu_set.hpp"

namespace pman {

enum class SchedulingPolicy {
    Other = SCHED_OTHER,
    Fifo = SCHED_FIFO,
    RoundRobin = SCHED_RR,
};

struct ThreadAttributes {
    std::optional<std::size_t> stackSize;
    std::optional<std::size_t> guardSize;
    std::optional<SchedulingPolicy> policy;
    std::optional<int> priority;
    std::optional<CpuSet> affinity;
    std::optional<int> numaNode;
    bool detached{false};
};

}  // namespace pman
