/// @file async_benchmark.cpp
/// @brief Performance benchmarks for async library

#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include <iostream>
#include <chrono>
#include <atomic>
#include <iomanip>

using namespace pman::async;

namespace {
template<typename Func>
auto benchmark(const char* name, Func&& func) {
    std::cout << std::setw(40) << std::left << name << std::flush;

    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << std::setw(12) << std::right << duration.count() << " μs\n";

    return duration.count();
}

void separator() {
    std::cout << std::string(60, '-') << "\n";
}
}

int main() {
    std::cout << "PMAN Async Performance Benchmarks\n";
    std::cout << "==================================\n\n";

    // Benchmark 1: Event loop creation overhead
    separator();
    std::cout << "Event Loop Creation Overhead:\n";
    separator();
    benchmark("Create and destroy event loop", []() {
        for (int i = 0; i < 1000; i++) {
            EventLoop loop;
        }
    });

    // Benchmark 2: Timer creation and firing
    separator();
    std::cout << "\nTimer Operations:\n";
    separator();
    benchmark("Add 1000 timers", []() {
        EventLoop loop;
        for (int i = 0; i < 1000; i++) {
            loop.addTimer(std::chrono::milliseconds(10), []() {});
        }
    });

    benchmark("Fire 100 immediate timers", []() {
        EventLoop loop;
        std::atomic<int> count{0};
        for (int i = 0; i < 100; i++) {
            loop.addTimer(std::chrono::nanoseconds(0), [&]() {
                count++;
                if (count == 100) loop.stop();
            });
        }
        loop.run();
    });

    benchmark("Fire 1000 immediate timers", []() {
        EventLoop loop;
        std::atomic<int> count{0};
        for (int i = 0; i < 1000; i++) {
            loop.addTimer(std::chrono::nanoseconds(0), [&]() {
                count++;
                if (count == 1000) loop.stop();
            });
        }
        loop.run();
    });

    // Benchmark 3: Coroutine operations
    separator();
    std::cout << "\nCoroutine Operations:\n";
    separator();
    benchmark("Create 1000 coroutines", []() {
        for (int i = 0; i < 1000; i++) {
            auto task = []() -> Task<void> {
                co_return;
            };
            auto t = task();
            // Task is created but not started
        }
    });

    benchmark("Run 100 simple coroutines", []() {
        EventLoop loop;
        std::atomic<int> count{0};

        for (int i = 0; i < 100; i++) {
            auto task = [&]() -> Task<void> {
                co_await sleep(Duration::fromNanos(0), &loop);
                count++;
                if (count == 100) loop.stop();
            };
            task().start();
        }
        loop.run();
    });

    benchmark("Run 1000 simple coroutines", []() {
        EventLoop loop;
        std::atomic<int> count{0};

        for (int i = 0; i < 1000; i++) {
            auto task = [&]() -> Task<void> {
                co_await sleep(Duration::fromNanos(0), &loop);
                count++;
                if (count == 1000) loop.stop();
            };
            task().start();
        }
        loop.run();
    });

    // Benchmark 4: Event loop iteration
    separator();
    std::cout << "\nEvent Loop Iterations:\n";
    separator();
    benchmark("1000 empty runOnce() calls", []() {
        EventLoop loop;
        for (int i = 0; i < 1000; i++) {
            loop.runOnce(std::chrono::nanoseconds(0));
        }
    });

    benchmark("1000 runOnce() with 1 timer each", []() {
        EventLoop loop;
        for (int i = 0; i < 1000; i++) {
            loop.addTimer(std::chrono::nanoseconds(0), []() {});
            loop.runOnce(std::chrono::milliseconds(1));
        }
    });

    // Benchmark 5: Post operations
    separator();
    std::cout << "\nPost Operations:\n";
    separator();
    benchmark("Post 100 callbacks", []() {
        EventLoop loop;
        std::atomic<int> count{0};

        for (int i = 0; i < 100; i++) {
            loop.post([&]() {
                count++;
                if (count == 100) loop.stop();
            });
        }
        loop.run();
    });

    benchmark("Post 1000 callbacks", []() {
        EventLoop loop;
        std::atomic<int> count{0};

        for (int i = 0; i < 1000; i++) {
            loop.post([&]() {
                count++;
                if (count == 1000) loop.stop();
            });
        }
        loop.run();
    });

    // Benchmark 6: Mixed workload
    separator();
    std::cout << "\nMixed Workload:\n";
    separator();
    benchmark("100 timers + 100 coroutines", []() {
        EventLoop loop;
        std::atomic<int> count{0};
        const int TARGET = 200;

        // Add timers
        for (int i = 0; i < 100; i++) {
            loop.addTimer(std::chrono::milliseconds(i % 10), [&]() {
                count++;
                if (count == TARGET) loop.stop();
            });
        }

        // Add coroutines
        for (int i = 0; i < 100; i++) {
            auto task = [&, i]() -> Task<void> {
                co_await sleep(Duration::fromMillis(i % 10), &loop);
                count++;
                if (count == TARGET) loop.stop();
            };
            task().start();
        }

        loop.run();
    });

    // Benchmark 7: Throughput test
    separator();
    std::cout << "\nThroughput Tests:\n";
    separator();

    auto throughput_10k = benchmark("Process 10,000 operations", []() {
        EventLoop loop;
        std::atomic<int> count{0};
        const int TARGET = 10000;

        for (int i = 0; i < TARGET; i++) {
            loop.addTimer(std::chrono::nanoseconds(0), [&]() {
                count++;
                if (count == TARGET) loop.stop();
            });
        }
        loop.run();
    });

    std::cout << "    Throughput: "
              << std::fixed << std::setprecision(2)
              << (10000.0 / throughput_10k * 1000000.0)
              << " ops/sec\n";

    // Benchmark 8: Latency test
    separator();
    std::cout << "\nLatency Tests:\n";
    separator();
    benchmark("Timer latency (10ms timer)", []() {
        EventLoop loop;

        auto start = std::chrono::high_resolution_clock::now();
        loop.addTimer(std::chrono::milliseconds(10), [&]() {
            auto end = std::chrono::high_resolution_clock::now();
            auto actual = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            std::cout << "\n    Actual time: " << actual.count() << " μs (expected ~10000 μs)\n    ";
            loop.stop();
        });
        loop.run();
    });

    benchmark("Coroutine wake latency", []() {
        EventLoop loop;

        auto start = std::chrono::high_resolution_clock::now();
        auto task = [&]() -> Task<void> {
            co_await sleep(Duration::fromMillis(10), &loop);
            auto end = std::chrono::high_resolution_clock::now();
            auto actual = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            std::cout << "\n    Actual time: " << actual.count() << " μs (expected ~10000 μs)\n    ";
            loop.stop();
        };
        task().start();
        loop.run();
    });

    separator();
    std::cout << "\nBenchmark complete!\n";

    return 0;
}
