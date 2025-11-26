#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "pman/observability.hpp"
#include "pman/thread_group.hpp"

int main() {
    pman::ThreadGroup group;
    for (int i = 0; i < 2; ++i) {
        group.add("worker-" + std::to_string(i), [i]() {
            for (int j = 0; j < 5; ++j) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50 + i * 10));
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    auto snapshot = pman::snapshotProcess();
    std::cout << "Process " << snapshot.pid << " (" << snapshot.name << ") has "
              << snapshot.threads.size() << " threads tracked\n";
    for (const auto& thread : snapshot.threads) {
        std::cout << "  tid=" << thread.tid << " state=" << thread.state
                  << " uticks=" << thread.userTimeTicks
                  << " stks=" << thread.systemTimeTicks << "\n";
    }

    group.joinAll();
    return 0;
}
