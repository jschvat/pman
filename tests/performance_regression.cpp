/// @file performance_regression.cpp
/// @brief Performance regression tests with baseline tracking
///
/// This test suite establishes performance baselines and detects regressions.
/// Run with --update to update baselines, otherwise compares against stored values.

#include <iostream>
#include <fstream>
#include <map>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <future>
#include <functional>

// Async components
#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"

// IPC components
#include "pman/pipe.hpp"

// Thread components
#include "pman/thread_pool.hpp"

// Sync primitives
#include "pman/semaphore.hpp"
#include "pman/barrier.hpp"
#include "pman/futex.hpp"

using namespace pman;
using namespace pman::async;

namespace {

// Baseline file path
const char* BASELINE_FILE = "performance_baseline.txt";

// Tolerance for regression (30% slower is a regression)
constexpr double REGRESSION_THRESHOLD = 0.30;

// Minimum runs for stable measurement
constexpr int MIN_RUNS = 5;

// Stored baselines
std::map<std::string, double> baselines;
std::map<std::string, double> current_results;

bool update_mode = false;
int regressions = 0;
int improvements = 0;

void load_baselines() {
    std::ifstream file(BASELINE_FILE);
    if (!file) return;

    std::string name;
    double value;
    while (file >> std::quoted(name) >> value) {
        baselines[name] = value;
    }
}

void save_baselines() {
    std::ofstream file(BASELINE_FILE);
    for (const auto& [name, value] : current_results) {
        file << std::quoted(name) << " " << std::fixed << std::setprecision(2) << value << "\n";
    }
}

template<typename Func>
double benchmark(const std::string& name, Func&& func, int iterations = MIN_RUNS) {
    std::vector<double> times;
    times.reserve(iterations);

    // Warmup
    func();

    // Measure
    for (int i = 0; i < iterations; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        times.push_back(static_cast<double>(us));
    }

    // Calculate median (more stable than mean)
    std::sort(times.begin(), times.end());
    double median = times[times.size() / 2];

    current_results[name] = median;

    // Compare to baseline
    std::cout << std::setw(50) << std::left << name;
    std::cout << std::setw(12) << std::right << std::fixed << std::setprecision(1) << median << " μs";

    auto it = baselines.find(name);
    if (it != baselines.end()) {
        double baseline = it->second;
        double change = (median - baseline) / baseline;

        if (change > REGRESSION_THRESHOLD) {
            std::cout << "  [REGRESSION +" << std::setprecision(0) << (change * 100) << "%]";
            regressions++;
        } else if (change < -REGRESSION_THRESHOLD) {
            std::cout << "  [IMPROVED " << std::setprecision(0) << (-change * 100) << "%]";
            improvements++;
        } else {
            std::cout << "  [OK]";
        }
    } else {
        std::cout << "  [NEW]";
    }

    std::cout << "\n";
    return median;
}

void section(const char* name) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << name << "\n";
    std::cout << std::string(70, '=') << "\n";
}

} // anonymous namespace

//=============================================================================
// TIMER BENCHMARKS
//=============================================================================

void benchmark_timers() {
    section("TIMER PERFORMANCE");

    // Single timer add/fire latency
    benchmark("timer_single_add_fire", []() {
        EventLoop loop;
        bool fired = false;
        loop.addTimer(std::chrono::nanoseconds(0), [&]() {
            fired = true;
            loop.stop();
        });
        loop.run();
    });

    // Bulk timer add (10,000 timers)
    benchmark("timer_bulk_add_10k", []() {
        EventLoop loop;
        for (int i = 0; i < 10000; i++) {
            loop.addTimer(std::chrono::milliseconds(100), []() {});
        }
    });

    // Timer cancellation
    benchmark("timer_add_cancel_1k", []() {
        EventLoop loop;
        std::vector<uint64_t> ids;
        ids.reserve(1000);
        for (int i = 0; i < 1000; i++) {
            ids.push_back(loop.addTimer(std::chrono::milliseconds(100), []() {}));
        }
        for (auto id : ids) {
            loop.cancelTimer(id);
        }
    });

    // Periodic timer tick latency
    benchmark("timer_periodic_100_ticks", []() {
        EventLoop loop;
        int count = 0;
        loop.addPeriodicTimer(std::chrono::microseconds(100), [&]() {
            if (++count >= 100) loop.stop();
        });
        loop.run();
    });
}

//=============================================================================
// COROUTINE BENCHMARKS
//=============================================================================

void benchmark_coroutines() {
    section("COROUTINE PERFORMANCE");

    // Coroutine creation and completion
    benchmark("coro_spawn_complete_100", []() {
        EventLoop loop;
        std::atomic<int> count{0};
        const int TARGET = 100;

        auto task = [](std::atomic<int>* count) -> Task<void> {
            (*count)++;
            co_return;
        };

        for (int i = 0; i < TARGET; i++) {
            task(&count).start();
        }

        // Add timer to stop loop after coroutines complete
        loop.addTimer(std::chrono::milliseconds(10), [&]() { loop.stop(); });
        loop.run();
    });

    // Coroutine with sleep
    benchmark("coro_sleep_10", []() {
        EventLoop loop;
        std::atomic<int> count{0};
        const int TARGET = 10;

        auto task = [](EventLoop* loop, std::atomic<int>* count) -> Task<void> {
            co_await sleep(Duration::fromMicros(100), loop);
            (*count)++;
        };

        for (int i = 0; i < TARGET; i++) {
            task(&loop, &count).start();
        }

        // Add timer to stop loop after coroutines complete
        loop.addTimer(std::chrono::milliseconds(10), [&]() { loop.stop(); });
        loop.run();
    });
}

//=============================================================================
// PIPE BENCHMARKS
//=============================================================================

void benchmark_pipes() {
    section("PIPE PERFORMANCE");

    // Pipe create/destroy
    benchmark("pipe_create_destroy", []() {
        for (int i = 0; i < 100; i++) {
            auto pipe = Pipe::create();
            (void)pipe;
        }
    });

    // Pipe throughput (1MB)
    benchmark("pipe_write_1mb", []() {
        auto pipe = Pipe::create();
        std::vector<char> data(1024 * 1024, 'x');
        size_t total_written = 0;

        std::thread reader([&]() {
            char buf[65536];
            while (true) {
                auto n = ::read(pipe.readEnd(), buf, sizeof(buf));
                if (n <= 0) break;
            }
        });

        // Write in chunks
        size_t offset = 0;
        while (offset < data.size()) {
            auto n = ::write(pipe.writeEnd(), data.data() + offset,
                           std::min(size_t(65536), data.size() - offset));
            if (n > 0) {
                offset += n;
                total_written += n;
            }
        }
        pipe.closeWrite();
        reader.join();
    });
}

//=============================================================================
// THREAD POOL BENCHMARKS
//=============================================================================

void benchmark_thread_pool() {
    section("THREAD POOL PERFORMANCE");

    // Task submission throughput
    benchmark("pool_submit_10k_tasks", []() {
        ThreadPool pool(4);
        std::atomic<int> count{0};

        for (int i = 0; i < 10000; i++) {
            pool.submit([&]() {
                count++;
            });
        }

        // Wait for completion
        while (count < 10000) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Mixed compute tasks
    benchmark("pool_mixed_100_tasks", []() {
        ThreadPool pool(4);
        std::atomic<int> sum{0};

        for (int i = 0; i < 100; i++) {
            pool.submit([i, &sum]() {
                // Small compute task
                int partial = 0;
                for (int j = 0; j < 1000; j++) {
                    partial += j;
                }
                sum += partial + i;
            });
        }

        // Wait for completion
        while (sum < 49500000) { // Approximate expected value
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
}

//=============================================================================
// SYNC PRIMITIVE BENCHMARKS
//=============================================================================

void benchmark_sync_primitives() {
    section("SYNC PRIMITIVE PERFORMANCE");

    // FutexMutex lock/unlock
    benchmark("futex_mutex_lock_unlock_10k", []() {
        FutexMutex mutex;
        int counter = 0;
        for (int i = 0; i < 10000; i++) {
            mutex.lock();
            counter++;
            mutex.unlock();
        }
        (void)counter;
    });

    // FutexSemaphore acquire/release
    benchmark("futex_semaphore_acq_rel_10k", []() {
        FutexSemaphore sem(1);
        int counter = 0;
        for (int i = 0; i < 10000; i++) {
            sem.acquire();
            counter++;
            sem.release();
        }
        (void)counter;
    });

    // Barrier synchronization
    benchmark("barrier_sync_4_threads_100", []() {
        Barrier barrier(4);
        std::vector<std::thread> threads;

        for (int i = 0; i < 4; i++) {
            threads.emplace_back([&]() {
                for (int j = 0; j < 100; j++) {
                    barrier.wait();
                }
            });
        }

        for (auto& t : threads) t.join();
    });

    // Contended mutex
    benchmark("futex_mutex_contended_4_threads", []() {
        FutexMutex mutex;
        std::atomic<int> counter{0};
        std::vector<std::thread> threads;

        for (int i = 0; i < 4; i++) {
            threads.emplace_back([&]() {
                for (int j = 0; j < 1000; j++) {
                    mutex.lock();
                    counter++;
                    mutex.unlock();
                }
            });
        }

        for (auto& t : threads) t.join();
    });
}

//=============================================================================
// LATENCY BENCHMARKS
//=============================================================================

void benchmark_latency() {
    section("LATENCY BENCHMARKS");

    // Event loop iteration latency
    benchmark("event_loop_iteration_empty", []() {
        EventLoop loop;
        for (int i = 0; i < 1000; i++) {
            loop.runOnce(std::chrono::milliseconds(0));
        }
    });

    // Timer fire latency (measure from add to callback)
    benchmark("timer_fire_latency_avg", []() {
        EventLoop loop;
        int count = 0;
        const int TARGET = 100;

        std::function<void()> add_timer;
        add_timer = [&]() {
            loop.addTimer(std::chrono::nanoseconds(0), [&]() {
                count++;
                if (count < TARGET) {
                    add_timer();
                } else {
                    loop.stop();
                }
            });
        };

        add_timer();
        loop.run();
    });
}

//=============================================================================
// MAIN
//=============================================================================

int main(int argc, char* argv[]) {
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║         PMAN Performance Regression Test Suite                   ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--update") {
            update_mode = true;
            std::cout << "\n[UPDATE MODE] Baselines will be updated\n";
        }
    }

    load_baselines();

    // Run all benchmarks
    benchmark_timers();
    benchmark_coroutines();
    benchmark_pipes();
    benchmark_thread_pool();
    benchmark_sync_primitives();
    benchmark_latency();

    // Summary
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "SUMMARY\n";
    std::cout << std::string(70, '=') << "\n";

    std::cout << "Total benchmarks: " << current_results.size() << "\n";

    if (!baselines.empty()) {
        std::cout << "Regressions:      " << regressions << "\n";
        std::cout << "Improvements:     " << improvements << "\n";
    }

    if (update_mode) {
        save_baselines();
        std::cout << "\nBaselines saved to " << BASELINE_FILE << "\n";
    }

    // Exit with error if regressions detected
    if (regressions > 0 && !update_mode) {
        std::cout << "\n[FAIL] Performance regressions detected!\n";
        return 1;
    }

    std::cout << "\n[PASS] Performance is acceptable\n";
    return 0;
}
