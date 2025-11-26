#include "pman/thread_config.hpp"

#include <errno.h>
#include <sys/mman.h>

#include <stdexcept>
#include <system_error>

#include "pman/topology.hpp"

namespace pman {
namespace {

void applyAffinity(pthread_t handle, const CpuSet& set) {
#ifdef __linux__
    if (pthread_setaffinity_np(handle, sizeof(cpu_set_t), set.data()) != 0) {
        throw std::system_error(errno, std::generic_category(), "pthread_setaffinity_np");
    }
#else
    (void)handle;
    (void)set;
#endif
}

CpuSet buildSetFromList(const std::vector<int>& cpus) {
    if (cpus.empty()) {
        throw std::invalid_argument("cpu list must not be empty");
    }
    CpuSet set;
    for (int cpu : cpus) {
        set.add(cpu);
    }
    return set;
}

}  // namespace

void configure_thread(pthread_t handle, const ThreadOptions& options) {
    CpuSet derived;
    const CpuSet* target = nullptr;

    if (options.cpus && !options.cpus->empty()) {
        derived = buildSetFromList(*options.cpus);
        target = &derived;
    } else if (options.numaNode) {
        derived = mask_for_node(*options.numaNode);
        target = &derived;
    }

    if (target) {
        applyAffinity(handle, *target);
    }

    if (options.schedFifoPriority) {
        sched_param param{};
        param.sched_priority = *options.schedFifoPriority;
        if (pthread_setschedparam(handle, SCHED_FIFO, &param) != 0) {
            throw std::system_error(errno, std::generic_category(), "pthread_setschedparam");
        }
    }

    if (options.lockMemory) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            throw std::system_error(errno, std::generic_category(), "mlockall");
        }
    }
}

void configure_current_thread(const ThreadOptions& options) {
    configure_thread(pthread_self(), options);
}

CpuSet mask_for_core(int coreId) {
    auto& topo = SystemTopology::instance();
    CpuSet set;
    for (int cpu : topo.siblingsForCore(coreId)) {
        set.add(cpu);
    }
    return set;
}

CpuSet mask_for_node(int nodeId) {
    return SystemTopology::instance().cpuSetForNode(nodeId);
}

}  // namespace pman
