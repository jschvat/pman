/// @file timeout_demo.cpp
/// @brief Demonstrates timeout behavior - task continues after timeout

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"
#include <iostream>
#include <chrono>

using namespace pman::async;

// A slow task that outputs progress
Task<int> slowTask(EventLoop* loop) {
    std::cout << "Task started!" << std::endl;

    for (int i = 1; i <= 10; i++) {
        std::cout << "  Working... iteration " << i << "/10" << std::endl;
        co_await sleep(Duration::fromSeconds(1), loop);
    }

    std::cout << "Task completed successfully!" << std::endl;
    co_return 42;
}

// Wrapper task that uses timeout
Task<void> demoTimeout(EventLoop* loop) {
    std::cout << "\n=== Timeout Demo: 3 second timeout on 10 second task ===\n" << std::endl;

    try {
        int result = co_await timeout(slowTask(loop), Duration::fromSeconds(3), loop);
        std::cout << "\nTask completed with result: " << result << std::endl;
    } catch (const TimeoutError& e) {
        std::cout << "\n*** TIMEOUT OCCURRED after 3 seconds! ***" << std::endl;
        std::cout << "*** But watch - the task continues running... ***\n" << std::endl;
    }

    // Wait a bit more to show the task keeps running
    std::cout << "\nWaiting 8 more seconds to observe the background task..." << std::endl;
    co_await sleep(Duration::fromSeconds(8), loop);

    std::cout << "\n=== Demo complete ===\n" << std::endl;
    loop->stop();
}

int main() {
    EventLoop loop;

    std::cout << "Starting timeout demonstration..." << std::endl;
    std::cout << "This will show that the task continues outputting" << std::endl;
    std::cout << "even after the timeout fires.\n" << std::endl;

    demoTimeout(&loop).start();

    loop.run();

    return 0;
}
