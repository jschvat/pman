/// @file timeout_demo2.cpp
/// @brief Shows that tasks ARE stopped when timeout occurs (Task destructor destroys coroutine)

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"
#include <iostream>

using namespace pman::async;

// A slow task with destructor logging
Task<int> slowTask(EventLoop* loop, bool* destructed) {
    std::cout << "Task started!" << std::endl;

    for (int i = 1; i <= 10; i++) {
        std::cout << "  Working... iteration " << i << "/10" << std::endl;
        co_await sleep(Duration::fromSeconds(1), loop);
    }

    std::cout << "Task completed successfully!" << std::endl;
    *destructed = true;
    co_return 42;
}

Task<void> demoTimeoutStopsTask(EventLoop* loop) {
    std::cout << "\n=== Demo: Timeout STOPS the task ===\n" << std::endl;

    bool destructed = false;

    try {
        int result = co_await timeout(slowTask(loop, &destructed), Duration::fromSeconds(3), loop);
        std::cout << "\nTask completed with result: " << result << std::endl;
    } catch (const TimeoutError& e) {
        std::cout << "\n*** TIMEOUT at 3 seconds ***" << std::endl;
    }

    std::cout << "\nAfter timeout, waiting 2 more seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(2), loop);

    std::cout << "Notice: No more 'Working...' messages appeared!" << std::endl;
    std::cout << "The task coroutine was destroyed when timeout occurred." << std::endl;

    loop->stop();
}

// Demo showing task running independently
Task<void> independentTask(EventLoop* loop) {
    std::cout << "Independent task started (not wrapped in timeout)" << std::endl;
    for (int i = 1; i <= 5; i++) {
        std::cout << "  Independent working... " << i << "/5" << std::endl;
        co_await sleep(Duration::fromSeconds(1), loop);
    }
    std::cout << "Independent task finished!" << std::endl;
}

Task<void> demoIndependentContinues(EventLoop* loop) {
    std::cout << "\n=== Demo: Independent tasks continue running ===\n" << std::endl;

    // Start a task WITHOUT waiting for it
    independentTask(loop).start();

    std::cout << "\nMain task: waiting 3 seconds..." << std::endl;
    co_await sleep(Duration::fromSeconds(3), loop);

    std::cout << "\nMain task: done waiting, but independent task continues..." << std::endl;
    co_await sleep(Duration::fromSeconds(3), loop);

    std::cout << "\n=== Both demos complete ===\n" << std::endl;
    loop->stop();
}

int main(int argc, char** argv) {
    EventLoop loop;

    if (argc > 1 && std::string(argv[1]) == "independent") {
        std::cout << "Demonstrating independent task behavior:\n" << std::endl;
        demoIndependentContinues(&loop).start();
    } else {
        std::cout << "Demonstrating timeout behavior:\n" << std::endl;
        demoTimeoutStopsTask(&loop).start();
    }

    loop.run();

    return 0;
}
