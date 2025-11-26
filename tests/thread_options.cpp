#include <atomic>
#include <pthread.h>

#include "pman/thread.hpp"
#include "pman/thread_config.hpp"
#include "pman/topology.hpp"

int main() {
    const auto& topo = pman::SystemTopology::instance();
    auto cpus = topo.cpuIds();
    if (cpus.empty()) {
        return 0;
    }
    int targetCpu = cpus.front();
    std::atomic<bool> success{false};

    pman::ManagedThread thread("options-test", [&]() {
        pman::ThreadOptions options;
        options.cpus = std::vector<int>{targetCpu};
        pman::configure_current_thread(options);

        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (pthread_getaffinity_np(pthread_self(), sizeof(mask), &mask) != 0) {
            return;
        }
        if (CPU_ISSET(targetCpu, &mask)) {
            success = true;
        }
    });

    thread.join();
    return success ? 0 : 1;
}
