/// @file async_comprehensive.cpp
/// @brief Comprehensive test suite for async library functionality

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include <iostream>
#include <cassert>
#include <atomic>
#include <chrono>

using namespace pman::async;

namespace {
std::atomic<int> test_count{0};
std::atomic<int> test_passed{0};

void test(const char* name, bool result) {
    test_count++;
    if (result) {
        test_passed++;
        std::cout << "[PASS] " << name << "\n";
    } else {
        std::cerr << "[FAIL] " << name << "\n";
    }
}

void section(const char* name) {
    std::cout << "\n=== " << name << " ===\n";
}
}

int main() {
    std::cout << "PMAN Async Comprehensive Test Suite\n";
    std::cout << "====================================\n";

    // Test 1: Basic event loop creation
    section("Event Loop Creation");
    {
        EventLoop loop;
        test("Event loop created", true);
        test("Event loop not running initially", !loop.isRunning());
        test("No active FDs initially", loop.activeFds() == 0);
        test("No active timers initially", loop.activeTimers() == 0);
    }

    // Test 2: Single timer
    section("Single Timer");
    {
        EventLoop loop;
        bool fired = false;

        loop.addTimer(std::chrono::milliseconds(10), [&]() {
            fired = true;
            loop.stop();
        });

        test("Timer added", loop.activeTimers() == 1);
        loop.run();
        test("Timer fired", fired);
        test("Timer cleaned up", loop.activeTimers() == 0);
    }

    // Test 3: Multiple sequential timers
    section("Multiple Sequential Timers");
    {
        EventLoop loop;
        int count = 0;

        loop.addTimer(std::chrono::milliseconds(10), [&]() { count++; });
        loop.addTimer(std::chrono::milliseconds(20), [&]() { count++; });
        loop.addTimer(std::chrono::milliseconds(30), [&]() {
            count++;
            loop.stop();
        });

        test("Three timers added", loop.activeTimers() == 3);
        loop.run();
        test("All three timers fired", count == 3);
    }

    // Test 4: Timer ordering
    section("Timer Ordering");
    {
        EventLoop loop;
        std::string order;

        loop.addTimer(std::chrono::milliseconds(30), [&]() { order += "3"; });
        loop.addTimer(std::chrono::milliseconds(10), [&]() { order += "1"; });
        loop.addTimer(std::chrono::milliseconds(20), [&]() { order += "2"; });
        loop.addTimer(std::chrono::milliseconds(40), [&]() {
            order += "4";
            loop.stop();
        });

        loop.run();
        test("Timers fired in correct order", order == "1234");
    }

    // Test 5: Timer cancellation
    section("Timer Cancellation");
    {
        EventLoop loop;
        bool fired = false;

        auto id = loop.addTimer(std::chrono::milliseconds(100), [&]() {
            fired = true;
        });

        loop.addTimer(std::chrono::milliseconds(10), [&]() {
            loop.cancelTimer(id);
            loop.stop();
        });

        loop.run();
        test("Cancelled timer did not fire", !fired);
    }

    // Test 6: Periodic timer
    section("Periodic Timer");
    {
        EventLoop loop;
        int count = 0;

        loop.addPeriodicTimer(std::chrono::milliseconds(10), [&]() {
            count++;
            if (count >= 3) {
                loop.stop();
            }
        });

        loop.run();
        test("Periodic timer fired multiple times", count >= 3);
    }

    // Test 7: Simple coroutine task
    section("Simple Coroutine Task");
    {
        EventLoop loop;
        bool completed = false;

        auto task = [&]() -> Task<void> {
            co_await sleep(Duration::fromMillis(10), &loop);
            completed = true;
            loop.stop();
        };

        task().start();
        loop.run();
        test("Coroutine task completed", completed);
    }

    // Test 8: Multiple concurrent coroutines
    section("Multiple Concurrent Coroutines");
    {
        EventLoop loop;
        int count = 0;

        auto task1 = [&]() -> Task<void> {
            co_await sleep(Duration::fromMillis(10), &loop);
            count++;
            if (count == 3) loop.stop();
        };

        auto task2 = [&]() -> Task<void> {
            co_await sleep(Duration::fromMillis(15), &loop);
            count++;
            if (count == 3) loop.stop();
        };

        auto task3 = [&]() -> Task<void> {
            co_await sleep(Duration::fromMillis(20), &loop);
            count++;
            if (count == 3) loop.stop();
        };

        task1().start();
        task2().start();
        task3().start();

        loop.run();
        test("All concurrent coroutines completed", count == 3);
    }

    // Test 9: Nested coroutine calls
    section("Nested Coroutine Calls");
    {
        EventLoop loop;
        int depth = 0;

        auto inner = [&]() -> Task<int> {
            co_await sleep(Duration::fromMillis(10), &loop);
            depth++;
            co_return 42;
        };

        auto outer = [&](auto inner_task) -> Task<void> {
            depth++;
            int result = co_await inner_task;
            test("Inner task returned correct value", result == 42);
            loop.stop();
        };

        outer(inner()).start();
        loop.run();
        test("Both inner and outer tasks executed", depth == 2);
    }

    // Test 10: Stress test - many concurrent operations
    section("Stress Test - 100 Concurrent Operations");
    {
        EventLoop loop;
        std::atomic<int> count{0};
        const int TARGET = 100;

        for (int i = 0; i < TARGET; i++) {
            auto task = [&, i]() -> Task<void> {
                co_await sleep(Duration::fromMillis(i % 50), &loop);
                count++;
                if (count == TARGET) {
                    loop.stop();
                }
            };
            task().start();
        }

        auto start = std::chrono::steady_clock::now();
        loop.run();
        auto elapsed = std::chrono::steady_clock::now() - start;

        test("All 100 operations completed", count == TARGET);

        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::cout << "    Completed in " << ms << "ms\n";
        test("Completed in reasonable time", ms < 200); // Should take ~50ms, allow 200ms
    }

    // Test 11: Post callback
    section("Post Callback");
    {
        EventLoop loop;
        bool executed = false;

        loop.post([&]() {
            executed = true;
            loop.stop();
        });

        loop.run();
        test("Posted callback executed", executed);
    }

    // Test 12: RunOnce functionality
    section("RunOnce Functionality");
    {
        EventLoop loop;
        int count = 0;

        loop.addTimer(std::chrono::milliseconds(10), [&]() { count++; });
        loop.addTimer(std::chrono::milliseconds(20), [&]() { count++; });

        // Run multiple iterations
        for (int i = 0; i < 10; i++) {
            loop.runOnce(std::chrono::milliseconds(5));
        }

        test("RunOnce processed timers", count == 2);
    }

    // Test 13: Event loop stop
    section("Event Loop Stop");
    {
        EventLoop loop;
        bool stopped = false;

        loop.addTimer(std::chrono::milliseconds(10), [&]() {
            loop.stop();
            stopped = true;
        });

        loop.addTimer(std::chrono::milliseconds(100), [&]() {
            // Should never fire
            stopped = false;
        });

        loop.run();
        test("Event loop stopped correctly", stopped);
    }

    // Test 14: Exception handling in coroutines
    section("Exception Handling in Coroutines");
    {
        EventLoop loop;
        bool caught = false;

        auto task = [&]() -> Task<void> {
            co_await sleep(Duration::fromMillis(10), &loop);
            throw std::runtime_error("test exception");
        };

        auto wrapper = [&](auto t) -> Task<void> {
            try {
                co_await t;
            } catch (const std::runtime_error& e) {
                caught = true;
            }
            loop.stop();
        };

        wrapper(task()).start();
        loop.run();
        test("Exception caught in coroutine", caught);
    }

    // Test 15: Task with return value
    section("Task with Return Value");
    {
        EventLoop loop;
        int result = 0;

        auto task = [&]() -> Task<int> {
            co_await sleep(Duration::fromMillis(10), &loop);
            co_return 123;
        };

        auto consumer = [&](auto t) -> Task<void> {
            result = co_await t;
            loop.stop();
        };

        consumer(task()).start();
        loop.run();
        test("Task returned correct value", result == 123);
    }

    // Summary
    std::cout << "\n====================================\n";
    std::cout << "Tests passed: " << test_passed << " / " << test_count << "\n";

    if (test_passed == test_count) {
        std::cout << "ALL TESTS PASSED!\n";
        return 0;
    } else {
        std::cerr << "SOME TESTS FAILED!\n";
        return 1;
    }
}
