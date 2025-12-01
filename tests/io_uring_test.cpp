/// @file io_uring_test.cpp
/// @brief Test io_uring backend specifically

#include <iostream>
#include <atomic>
#include <chrono>

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"

using namespace pman::async;

namespace {

int tests_run = 0;
int tests_passed = 0;

void test(const char* name, bool result) {
    tests_run++;
    if (result) {
        tests_passed++;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cerr << "  [FAIL] " << name << "\n";
    }
}

} // namespace

void test_basic_timer() {
    std::cout << "\n--- Basic Timer Test ---\n";

    EventLoopConfig config;
    config.backend = BackendType::IoUring;
    EventLoop loop(config);

    test("Backend is io_uring", loop.backend() == BackendType::IoUring);

    bool fired = false;
    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        fired = true;
        loop.stop();
    });

    loop.run();
    test("Timer callback fired", fired);
}

void test_multiple_timers() {
    std::cout << "\n--- Multiple Timers Test ---\n";

    EventLoopConfig config;
    config.backend = BackendType::IoUring;
    EventLoop loop(config);

    std::atomic<int> count{0};
    const int N = 100;

    for (int i = 0; i < N; i++) {
        loop.addTimer(std::chrono::milliseconds(1), [&]() {
            count++;
        });
    }

    loop.addTimer(std::chrono::milliseconds(50), [&]() {
        loop.stop();
    });

    loop.run();
    test("All timers fired", count == N);
}

void test_timer_cancellation() {
    std::cout << "\n--- Timer Cancellation Test ---\n";

    EventLoopConfig config;
    config.backend = BackendType::IoUring;
    EventLoop loop(config);

    bool cancelled_fired = false;
    auto id = loop.addTimer(std::chrono::milliseconds(10), [&]() {
        cancelled_fired = true;
    });

    loop.cancelTimer(id);

    bool other_fired = false;
    loop.addTimer(std::chrono::milliseconds(20), [&]() {
        other_fired = true;
        loop.stop();
    });

    loop.run();
    test("Cancelled timer didn't fire", !cancelled_fired);
    test("Other timer fired", other_fired);
}

void test_periodic_timer() {
    std::cout << "\n--- Periodic Timer Test ---\n";

    EventLoopConfig config;
    config.backend = BackendType::IoUring;
    EventLoop loop(config);

    std::atomic<int> count{0};

    loop.addPeriodicTimer(std::chrono::milliseconds(5), [&]() {
        if (++count >= 10) {
            loop.stop();
        }
    });

    loop.run();
    test("Periodic timer fired 10 times", count >= 10);
}

void test_coroutine() {
    std::cout << "\n--- Coroutine Test ---\n";

    EventLoopConfig config;
    config.backend = BackendType::IoUring;
    EventLoop loop(config);

    std::atomic<int> count{0};

    auto task = [](EventLoop* loop, std::atomic<int>* count) -> Task<void> {
        co_await sleep(Duration::fromMillis(10), loop);
        (*count)++;
        loop->stop();
    };

    task(&loop, &count).start();
    loop.run();
    test("Coroutine completed", count == 1);
}

void test_timeout() {
    std::cout << "\n--- Timeout Test ---\n";

    EventLoopConfig config;
    config.backend = BackendType::IoUring;
    EventLoop loop(config);

    bool timed_out = false;

    auto slow = [](EventLoop* l) -> Task<void> {
        co_await sleep(Duration::fromSeconds(10), l);
    };

    auto runner = [&]() -> Task<void> {
        try {
            co_await timeout(slow(&loop), Duration::fromMillis(50), &loop);
        } catch (const TimeoutError&) {
            timed_out = true;
        }
        loop.stop();
    };

    runner().start();
    loop.run();
    test("Timeout triggered", timed_out);
}

void test_rapid_add_cancel() {
    std::cout << "\n--- Rapid Add/Cancel Test ---\n";

    EventLoopConfig config;
    config.backend = BackendType::IoUring;
    EventLoop loop(config);

    const int N = 1000;
    std::atomic<int> fired{0};
    std::atomic<int> cancelled{0};

    for (int i = 0; i < N; i++) {
        auto id = loop.addTimer(std::chrono::milliseconds(100), [&]() {
            fired++;
        });
        if (i % 2 == 0) {
            loop.cancelTimer(id);
            cancelled++;
        }
    }

    loop.addTimer(std::chrono::milliseconds(200), [&]() {
        loop.stop();
    });

    loop.run();
    test("Correct number fired", fired == N - cancelled);
    test("Half cancelled", cancelled == N / 2);
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           io_uring Backend Test Suite                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    try {
        test_basic_timer();
        test_multiple_timers();
        test_timer_cancellation();
        test_periodic_timer();
        test_coroutine();
        test_timeout();
        test_rapid_add_cancel();
    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] Exception: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n========================================\n";
    std::cout << "RESULTS\n";
    std::cout << "========================================\n";
    std::cout << "Passed: " << tests_passed << "/" << tests_run << "\n";

    return tests_passed == tests_run ? 0 : 1;
}
