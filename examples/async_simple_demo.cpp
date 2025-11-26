/// @file async_simple_demo.cpp
/// @brief Simple working demo of async functions and timers

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include <iostream>

using namespace pman::async;

int main() {
    std::cout << "\n╔════════════════════════════════════════════════╗\n";
    std::cout << "║   PMAN ASYNC SIMPLE DEMO                       ║\n";
    std::cout << "╚════════════════════════════════════════════════╝\n\n";

    // Demo 1: Basic Sleep
    std::cout << "=== Demo 1: Basic Sleep ===\n";
    {
        EventLoop loop;

        auto task = [](EventLoop* loop) -> Task<void> {
            std::cout << "Starting countdown...\n";
            for (int i = 3; i > 0; --i) {
                std::cout << i << "...\n" << std::flush;
                co_await sleep(Duration::fromSeconds(1), loop);
            }
            std::cout << "Blast off!\n";
            loop->stop();
        };

        task(&loop).start();  // Self-managing Task - automatically cleans up
        loop.run();
        std::cout << "Demo 1 complete!\n\n";
    }

    // Demo 2: Multiple concurrent sleeps
    std::cout << "=== Demo 2: Concurrent Sleeps ===\n";
    {
        EventLoop loop;
        int completed = 0;

        auto task1 = [](EventLoop* loop, int* counter) -> Task<void> {
            std::cout << "Task 1 sleeping for 500ms...\n";
            co_await sleep(Duration::fromMillis(500), loop);
            std::cout << "Task 1 done!\n";
            (*counter)++;
            if (*counter == 2) loop->stop();
        };

        auto task2 = [](EventLoop* loop, int* counter) -> Task<void> {
            std::cout << "Task 2 sleeping for 800ms...\n";
            co_await sleep(Duration::fromMillis(800), loop);
            std::cout << "Task 2 done!\n";
            (*counter)++;
            if (*counter == 2) loop->stop();
        };

        task1(&loop, &completed).start();
        task2(&loop, &completed).start();
        loop.run();
        std::cout << "Demo 2 complete! Both tasks finished.\n\n";
    }

    std::cout << "╔════════════════════════════════════════════════╗\n";
    std::cout << "║   ALL DEMOS COMPLETED SUCCESSFULLY!           ║\n";
    std::cout << "╚════════════════════════════════════════════════╝\n\n";

    return 0;
}
