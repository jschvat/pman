/// @file race_simple_test.cpp
/// @brief Simple test to verify race() allows task to continue

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"
#include <iostream>
#include <atomic>

using namespace pman::async;

std::atomic<int> taskIterations{0};

Task<void> backgroundTask(EventLoop* loop) {
    std::cout << "[BG Task] Started" << std::endl;

    for (int i = 1; i <= 10; i++) {
        taskIterations = i;
        std::cout << "[BG Task] Iteration " << i << "/10" << std::endl;
        co_await sleep(Duration::fromSeconds(1), loop);
    }

    std::cout << "[BG Task] COMPLETED ALL 10 ITERATIONS!" << std::endl;
}

Task<void> mainTask(EventLoop* loop) {
    std::cout << "\n[Main] Starting race() with 3 second timeout..." << std::endl;

    bool completed = co_await race(backgroundTask(loop), Duration::fromSeconds(3), loop);

    std::cout << "\n[Main] race() returned: " << (completed ? "completed" : "timed out") << std::endl;
    std::cout << "[Main] Task iterations so far: " << taskIterations.load() << std::endl;

    std::cout << "\n[Main] Waiting 8 more seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(8), loop);

    std::cout << "\n[Main] Final iteration count: " << taskIterations.load() << std::endl;

    if (taskIterations.load() == 10) {
        std::cout << "[Main] SUCCESS! Background task completed all 10 iterations!" << std::endl;
    } else {
        std::cout << "[Main] Background task only completed " << taskIterations.load() << " iterations" << std::endl;
    }

    loop->stop();
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "race() Simple Test" << std::endl;
    std::cout << "========================================\n" << std::endl;

    EventLoop loop;
    mainTask(&loop).start();
    loop.run();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
