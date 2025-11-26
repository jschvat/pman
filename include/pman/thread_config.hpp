#pragma once

#include <optional>
#include <vector>

#include <pthread.h>

#include "pman/cpu_set.hpp"

namespace pman {

struct ThreadOptions {
    std::optional<std::vector<int>> cpus;
    std::optional<int> schedFifoPriority;
    std::optional<int> numaNode;
    bool lockMemory{false};
};

void configure_thread(pthread_t handle, const ThreadOptions& options);
void configure_current_thread(const ThreadOptions& options);

CpuSet mask_for_core(int coreId);
CpuSet mask_for_node(int nodeId);

}  // namespace pman
