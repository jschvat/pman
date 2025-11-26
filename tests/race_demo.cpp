/// @file race_demo.cpp
/// @brief Demonstrates race() - non-cancelling timeout (like Promise.race)

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"
#include <iostream>

using namespace pman::async;

// Slow task that outputs progress
Task<void> slowTask(EventLoop* loop) {
    std::cout << "[Slow Task] Starting..." << std::endl;

    for (int i = 1; i <= 10; i++) {
        std::cout << "[Slow Task] Iteration " << i << "/10" << std::endl;
        co_await sleep(Duration::fromSeconds(1), loop);
    }

    std::cout << "[Slow Task] Completed!" << std::endl;
}

// Demo 1: Using race() - task continues after timeout (like Node.js)
Task<void> demoRace(EventLoop* loop) {
    std::cout << "\n=== Demo: race() - Task continues after timeout ===\n" << std::endl;
    std::cout << "This behaves like JavaScript's Promise.race()\n" << std::endl;

    bool completed = co_await race(slowTask(loop), Duration::fromSeconds(3), loop);

    if (completed) {
        std::cout << "\n[Main] Task completed before timeout" << std::endl;
    } else {
        std::cout << "\n[Main] TIMEOUT at 3 seconds!" << std::endl;
        std::cout << "[Main] But the task CONTINUES running in background..." << std::endl;
    }

    std::cout << "\n[Main] Waiting 8 more seconds to observe background task..." << std::endl;
    co_await sleep(Duration::fromSeconds(8), loop);

    std::cout << "\n[Main] Notice: Task kept printing even after timeout!\n" << std::endl;
    loop->stop();
}

// Demo 2: Compare timeout() vs race()
Task<void> taskWithTimeout(EventLoop* loop) {
    std::cout << "[timeout() Task] Starting..." << std::endl;

    for (int i = 1; i <= 10; i++) {
        std::cout << "[timeout() Task] Iteration " << i << "/10" << std::endl;
        co_await sleep(Duration::fromSeconds(1), loop);
    }

    std::cout << "[timeout() Task] Completed!" << std::endl;
}

Task<void> taskWithRace(EventLoop* loop) {
    std::cout << "[race() Task] Starting..." << std::endl;

    for (int i = 1; i <= 10; i++) {
        std::cout << "[race() Task] Iteration " << i << "/10" << std::endl;
        co_await sleep(Duration::fromSeconds(1), loop);
    }

    std::cout << "[race() Task] Completed!" << std::endl;
}

Task<void> demoComparison(EventLoop* loop) {
    std::cout << "\n=== Comparison: timeout() vs race() ===\n" << std::endl;

    std::cout << "--- Starting timeout() test ---\n" << std::endl;

    try {
        co_await timeout(taskWithTimeout(loop), Duration::fromSeconds(3), loop);
        std::cout << "\n[Main] timeout() task completed" << std::endl;
    } catch (const TimeoutError&) {
        std::cout << "\n[Main] timeout() fired - task was DESTROYED" << std::endl;
    }

    std::cout << "\n[Main] Waiting 3 seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(3), loop);
    std::cout << "[Main] Notice: timeout() task stopped printing\n" << std::endl;

    std::cout << "\n--- Starting race() test ---\n" << std::endl;

    bool completed = co_await race(taskWithRace(loop), Duration::fromSeconds(3), loop);

    if (completed) {
        std::cout << "\n[Main] race() task completed" << std::endl;
    } else {
        std::cout << "\n[Main] race() timeout - task CONTINUES" << std::endl;
    }

    std::cout << "\n[Main] Waiting 8 seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(8), loop);
    std::cout << "\n[Main] Notice: race() task kept printing!\n" << std::endl;

    loop->stop();
}

int main(int argc, char** argv) {
    EventLoop loop;

    std::cout << "\n========================================" << std::endl;
    std::cout << "race() Function Demo" << std::endl;
    std::cout << "Promise.race() behavior in C++" << std::endl;
    std::cout << "========================================" << std::endl;

    if (argc > 1 && std::string(argv[1]) == "compare") {
        demoComparison(&loop).start();
    } else {
        std::cout << "\nUsage:" << std::endl;
        std::cout << "  ./pman_race_demo           - Show race() behavior" << std::endl;
        std::cout << "  ./pman_race_demo compare   - Compare timeout() vs race()\n" << std::endl;

        if (argc == 1) {
            demoRace(&loop).start();
        }
    }

    loop.run();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Demo Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
