/// @file timeout_comparison.cpp
/// @brief Shows the difference between timeout() and independent tasks

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"
#include <iostream>

using namespace pman::async;

// Scenario 1: Task wrapped in timeout() - STOPS when timeout fires
Task<void> scenario1(EventLoop* loop) {
    std::cout << "\n=== SCENARIO 1: Task wrapped in timeout() ===\n" << std::endl;

    auto slowTask = [&]() -> Task<int> {
        std::cout << "[Wrapped Task] Starting..." << std::endl;

        for (int i = 1; i <= 10; i++) {
            std::cout << "[Wrapped Task] Iteration " << i << "/10" << std::endl;
            co_await sleep(Duration::fromSeconds(1), loop);
        }

        std::cout << "[Wrapped Task] Completed!" << std::endl;
        co_return 42;
    };

    try {
        int result = co_await timeout(slowTask(), Duration::fromSeconds(3), loop);
        std::cout << "[Main] Task completed with result: " << result << std::endl;
    } catch (const TimeoutError& e) {
        std::cout << "[Main] TIMEOUT at 3 seconds!" << std::endl;
        std::cout << "[Main] The wrapped task was DESTROYED" << std::endl;
    }

    std::cout << "\n[Main] Waiting 3 more seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(3), loop);

    std::cout << "[Main] Notice: Wrapped task stopped printing!\n" << std::endl;
}

// Scenario 2: Task started independently - CONTINUES running
Task<void> independentTask(EventLoop* loop, int* counter) {
    std::cout << "[Independent Task] Starting..." << std::endl;

    for (int i = 1; i <= 10; i++) {
        std::cout << "[Independent Task] Iteration " << i << "/10" << std::endl;
        (*counter)++;
        co_await sleep(Duration::fromSeconds(1), loop);
    }

    std::cout << "[Independent Task] Completed!" << std::endl;
}

Task<void> scenario2(EventLoop* loop) {
    std::cout << "\n=== SCENARIO 2: Task started independently ===\n" << std::endl;

    int counter = 0;

    // Start task WITHOUT awaiting it
    independentTask(loop, &counter).start();

    std::cout << "[Main] Started independent task, now waiting 3 seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(3), loop);

    std::cout << "[Main] Main task done waiting after 3 seconds" << std::endl;
    std::cout << "[Main] Counter is: " << counter << std::endl;
    std::cout << "[Main] But independent task CONTINUES running!" << std::endl;

    std::cout << "\n[Main] Waiting 8 more seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(8), loop);

    std::cout << "[Main] Final counter: " << counter << std::endl;
    std::cout << "[Main] Independent task kept running!\n" << std::endl;
}

// Scenario 3: Using tryTimeout() - same behavior as timeout()
Task<void> scenario3(EventLoop* loop) {
    std::cout << "\n=== SCENARIO 3: Using tryTimeout() ===\n" << std::endl;

    auto slowTask = [&]() -> Task<int> {
        std::cout << "[tryTimeout Task] Starting..." << std::endl;

        for (int i = 1; i <= 10; i++) {
            std::cout << "[tryTimeout Task] Iteration " << i << "/10" << std::endl;
            co_await sleep(Duration::fromSeconds(1), loop);
        }

        std::cout << "[tryTimeout Task] Completed!" << std::endl;
        co_return 99;
    };

    auto result = co_await tryTimeout(slowTask(), Duration::fromSeconds(3), loop);

    if (result) {
        std::cout << "[Main] Got result: " << *result << std::endl;
    } else {
        std::cout << "[Main] TIMEOUT! (returned nullopt)" << std::endl;
        std::cout << "[Main] Task was DESTROYED" << std::endl;
    }

    std::cout << "\n[Main] Waiting 3 more seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(3), loop);

    std::cout << "[Main] Notice: tryTimeout task also stopped!\n" << std::endl;
}

Task<void> runDemo(EventLoop* loop, int scenario) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Timeout Behavior Comparison Demo" << std::endl;
    std::cout << "========================================" << std::endl;

    switch (scenario) {
        case 1:
            co_await scenario1(loop);
            break;
        case 2:
            co_await scenario2(loop);
            break;
        case 3:
            co_await scenario3(loop);
            break;
        default:
            std::cout << "\nUsage: " << std::endl;
            std::cout << "  ./pman_timeout_comparison 1  - Show timeout() stops task" << std::endl;
            std::cout << "  ./pman_timeout_comparison 2  - Show independent task continues" << std::endl;
            std::cout << "  ./pman_timeout_comparison 3  - Show tryTimeout() stops task" << std::endl;
            break;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "Demo Complete!" << std::endl;
    std::cout << "========================================\n" << std::endl;

    loop->stop();
}

int main(int argc, char** argv) {
    EventLoop loop;

    int scenario = (argc > 1) ? std::atoi(argv[1]) : 0;

    if (scenario == 0) {
        std::cout << "\nPlease choose a scenario:\n" << std::endl;
        std::cout << "  1 - timeout() STOPS the task when timeout fires" << std::endl;
        std::cout << "  2 - Independent tasks CONTINUE running" << std::endl;
        std::cout << "  3 - tryTimeout() also STOPS the task" << std::endl;
        std::cout << "\nExample: ./pman_timeout_comparison 1\n" << std::endl;
        return 0;
    }

    runDemo(&loop, scenario).start();
    loop.run();

    return 0;
}
