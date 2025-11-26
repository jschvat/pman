#include <atomic>
#include <chrono>
#include <sys/wait.h>

#include <thread>

#include "pman/process.hpp"
#include "pman/thread.hpp"
#include "pman/thread_pool.hpp"

int main() {
    std::atomic<int> counter{0};
    {
        pman::ManagedThread thread("smoke-thread", [&counter]() {
            ++counter;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
        thread.join();
    }
    if (counter != 1) {
        return 1;
    }

    pman::ThreadPool pool{2};
    std::atomic<int> poolHits{0};
    pool.submit([&poolHits]() {
        ++poolHits;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if (poolHits.load() == 0) {
        return 2;
    }

    pman::ProcessConfig config{
        .executable = "/bin/true",
        .arguments = {},
        .environment = {},
    };
    auto proc = pman::launchProcess(config);
    int status = proc.wait();
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 3;
    }

    return 0;
}
