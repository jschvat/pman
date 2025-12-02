/// @file combinators_test.cpp
/// @brief Test async combinators: all, any, first

#include <iostream>
#include <atomic>
#include <chrono>

#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/combinators.hpp"

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

// Simple awaiter for testing - adds timer directly
struct TestTimer {
    std::chrono::nanoseconds duration_;
    EventLoop* loop_;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        loop_->addTimer(duration_, [h]() { h.resume(); });
    }
    void await_resume() {}
};

} // namespace

void test_all_basic() {
    std::cout << "\n--- all() Basic Test ---\n";

    EventLoop loop;

    auto make_task = [&loop](int value, int delay_ms) -> Task<int> {
        co_await TestTimer{std::chrono::milliseconds(delay_ms), &loop};
        co_return value;
    };

    auto runner = [&loop, &make_task]() -> Task<void> {
        std::vector<Task<int>> tasks;
        tasks.push_back(make_task(1, 10));
        tasks.push_back(make_task(2, 20));
        tasks.push_back(make_task(3, 30));

        auto results = co_await all(std::move(tasks), &loop);

        test("all() returned 3 results", results.size() == 3);
        test("all() results correct", results[0] == 1 && results[1] == 2 && results[2] == 3);

        loop.stop();
    };

    runner().start();
    loop.run();
}

void test_all_void() {
    std::cout << "\n--- all() Void Tasks Test ---\n";

    EventLoop loop;
    std::atomic<int> count{0};

    auto make_task = [&loop, &count](int delay_ms) -> Task<void> {
        co_await TestTimer{std::chrono::milliseconds(delay_ms), &loop};
        count++;
    };

    auto runner = [&loop, &make_task, &count]() -> Task<void> {
        std::vector<Task<void>> tasks;
        tasks.push_back(make_task(10));
        tasks.push_back(make_task(20));
        tasks.push_back(make_task(30));

        co_await all(std::move(tasks), &loop);

        test("all() void tasks completed", count == 3);
        loop.stop();
    };

    runner().start();
    loop.run();
}

void test_any_basic() {
    std::cout << "\n--- any() Basic Test ---\n";

    EventLoop loop;

    auto make_task = [&loop](int value, int delay_ms) -> Task<int> {
        co_await TestTimer{std::chrono::milliseconds(delay_ms), &loop};
        co_return value;
    };

    auto runner = [&loop, &make_task]() -> Task<void> {
        std::vector<Task<int>> tasks;
        tasks.push_back(make_task(100, 100));  // slow
        tasks.push_back(make_task(10, 10));    // fast - should win
        tasks.push_back(make_task(50, 50));    // medium

        auto result = co_await any(std::move(tasks), &loop);

        test("any() winner index is 1 (fastest)", result.index == 1);
        test("any() winner value is 10", result.value == 10);

        loop.stop();
    };

    runner().start();
    loop.run();
}

void test_any_void() {
    std::cout << "\n--- any() Void Tasks Test ---\n";

    EventLoop loop;

    auto make_task = [&loop](int delay_ms) -> Task<void> {
        co_await TestTimer{std::chrono::milliseconds(delay_ms), &loop};
    };

    auto runner = [&loop, &make_task]() -> Task<void> {
        std::vector<Task<void>> tasks;
        tasks.push_back(make_task(100));  // slow
        tasks.push_back(make_task(10));   // fast - should win
        tasks.push_back(make_task(50));   // medium

        auto winnerIdx = co_await any(std::move(tasks), &loop);

        test("any() void winner index is 1 (fastest)", winnerIdx == 1);

        loop.stop();
    };

    runner().start();
    loop.run();
}

void test_first_basic() {
    std::cout << "\n--- first() Basic Test ---\n";

    EventLoop loop;

    auto make_task = [&loop](int value, int delay_ms) -> Task<int> {
        co_await TestTimer{std::chrono::milliseconds(delay_ms), &loop};
        co_return value;
    };

    auto runner = [&loop, &make_task]() -> Task<void> {
        std::vector<Task<int>> tasks;
        tasks.push_back(make_task(100, 100));
        tasks.push_back(make_task(5, 5));     // fastest
        tasks.push_back(make_task(50, 50));

        auto result = co_await first(std::move(tasks), &loop);

        test("first() returns fastest value", result == 5);

        loop.stop();
    };

    runner().start();
    loop.run();
}

void test_all_many_tasks() {
    std::cout << "\n--- all() Many Tasks Test ---\n";

    EventLoop loop;

    auto make_task = [&loop](int value) -> Task<int> {
        co_await TestTimer{std::chrono::milliseconds(1), &loop};
        co_return value;
    };

    auto runner = [&loop, &make_task]() -> Task<void> {
        const int N = 100;
        std::vector<Task<int>> tasks;
        for (int i = 0; i < N; i++) {
            tasks.push_back(make_task(i));
        }

        auto results = co_await all(std::move(tasks), &loop);

        bool correct = results.size() == static_cast<size_t>(N);
        for (int i = 0; correct && i < N; i++) {
            if (results[i] != i) correct = false;
        }

        test("all() 100 tasks returned correct results", correct);

        loop.stop();
    };

    runner().start();
    loop.run();
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           Async Combinators Test Suite                       ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    try {
        test_all_basic();
        test_all_void();
        test_any_basic();
        test_any_void();
        test_first_basic();
        test_all_many_tasks();
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
