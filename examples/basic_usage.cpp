#include <chrono>
#include <iostream>
#include <thread>

#include "pman/process.hpp"
#include "pman/thread_group.hpp"
#include "pman/thread_pool.hpp"

int main() {
    using namespace std::chrono_literals;

    std::cout << "Launching thread pool...\n";
    pman::ThreadPool pool{4};
    for (int i = 0; i < 6; ++i) {
        pool.submit([i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50 * (i + 1)));
            std::cout << "task " << i << " finished on thread\n";
        });
    }

    pman::ThreadGroup group;
    group.add("heartbeat", []() {
        for (int i = 0; i < 3; ++i) {
            std::cout << "[heartbeat] tick " << i << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    });

    group.joinAll();

    std::cout << "Spawning /bin/echo via ProcessHandle...\n";
    pman::ProcessConfig cfg{
        .executable = "/bin/echo",
        .arguments = {"hello from pman"},
        .environment = {},
    };
    auto process = pman::launchProcess(cfg);
    process.wait();

    std::cout << "Done.\n";
    return 0;
}
