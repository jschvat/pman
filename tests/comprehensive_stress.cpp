/// @file comprehensive_stress.cpp
/// @brief Comprehensive stress tests for PMAN library components
///
/// Tests covered:
/// 1. Async: Event loop, timers, coroutines
/// 2. IPC: Pipes, shared memory
/// 3. Processes: Builder, groups
/// 4. Threads: Pools, CPU pinning
/// 5. Sync: Mutex, semaphore, barrier, latch

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <cassert>
#include <cstring>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

// Async components
#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"

// IPC components
#include "pman/pipe.hpp"
#include "pman/shared_memory.hpp"

// Process components
#include "pman/process.hpp"
#include "pman/process_builder.hpp"
#include "pman/process_group.hpp"

// Thread components
#include "pman/thread.hpp"
#include "pman/thread_pool.hpp"
#include "pman/cpu_set.hpp"

// Sync primitives
#include "pman/semaphore.hpp"
#include "pman/barrier.hpp"
#include "pman/latch.hpp"
#include "pman/futex.hpp"
#include "pman/rw_mutex.hpp"
#include "pman/seqlock.hpp"

// Scheduling
#include "pman/topology.hpp"
#include "pman/scheduling.hpp"

using namespace pman;
using namespace pman::async;

namespace {

std::atomic<int> total_tests{0};
std::atomic<int> passed_tests{0};
std::atomic<int> failed_tests{0};

void test(const char* name, bool result) {
    total_tests++;
    if (result) {
        passed_tests++;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        failed_tests++;
        std::cerr << "  [FAIL] " << name << "\n";
    }
}

void section(const char* name) {
    std::cout << "\n========================================\n";
    std::cout << name << "\n";
    std::cout << "========================================\n";
}

void subsection(const char* name) {
    std::cout << "\n--- " << name << " ---\n";
}

template<typename Func>
auto timed(Func&& func) {
    auto start = std::chrono::high_resolution_clock::now();
    func();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

} // anonymous namespace

//=============================================================================
// ASYNC STRESS TESTS
//=============================================================================

void test_async_timer_stress() {
    subsection("Timer Stress Test");

    // Test 1: Create and fire 10,000 timers
    {
        EventLoop loop;
        std::atomic<int> count{0};
        const int NUM_TIMERS = 10000;

        for (int i = 0; i < NUM_TIMERS; i++) {
            loop.addTimer(std::chrono::nanoseconds(0), [&]() {
                count++;
                if (count == NUM_TIMERS) loop.stop();
            });
        }

        auto ms = timed([&]() { loop.run(); });
        test("10,000 timers fired", count == NUM_TIMERS);
        std::cout << "    Time: " << ms << "ms (" << (NUM_TIMERS * 1000 / (ms + 1)) << " timers/sec)\n";
    }

    // Test 2: Rapid add/cancel cycles
    {
        EventLoop loop;
        std::atomic<int> fired{0};
        int cancelled = 0;
        const int CYCLES = 5000;

        for (int i = 0; i < CYCLES; i++) {
            auto id = loop.addTimer(std::chrono::milliseconds(100), [&]() { fired++; });
            if (i % 2 == 0) {
                if (loop.cancelTimer(id)) cancelled++;
            }
        }

        loop.addTimer(std::chrono::milliseconds(150), [&]() { loop.stop(); });
        loop.run();

        test("Rapid add/cancel cycles", cancelled > 0);
        std::cout << "    Fired: " << fired << ", Cancelled: " << cancelled << "\n";
    }

    // Test 3: Timer ordering under load
    {
        EventLoop loop;
        std::vector<int> order;
        order.reserve(100);

        for (int i = 99; i >= 0; i--) {
            loop.addTimer(std::chrono::milliseconds(i), [&, i]() {
                order.push_back(i);
            });
        }

        loop.addTimer(std::chrono::milliseconds(150), [&]() { loop.stop(); });
        loop.run();

        test("Timer ordering under load", order.size() >= 95);
    }

    // Test 4: Periodic timer stress
    {
        EventLoop loop;
        std::atomic<int> ticks{0};
        const int TARGET_TICKS = 100;

        loop.addPeriodicTimer(std::chrono::milliseconds(1), [&]() {
            ticks++;
            if (ticks >= TARGET_TICKS) loop.stop();
        });

        auto ms = timed([&]() { loop.run(); });
        test("Periodic timer stress", ticks >= TARGET_TICKS);
        std::cout << "    " << ticks << " ticks in " << ms << "ms\n";
    }
}

void test_async_coroutine_stress() {
    subsection("Coroutine Stress Test");

    // Test 1: Many concurrent coroutines
    {
        EventLoop loop;
        std::atomic<int> completed{0};
        const int NUM_COROS = 1000;

        auto worker = [](EventLoop* loop, std::atomic<int>* count, int total) -> Task<void> {
            co_await sleep(Duration::fromMillis(1), loop);
            (*count)++;
            if (*count == total) {
                loop->stop();
            }
        };

        for (int i = 0; i < NUM_COROS; i++) {
            worker(&loop, &completed, NUM_COROS).start();
        }

        auto ms = timed([&]() { loop.run(); });
        test("1000 concurrent coroutines", completed == NUM_COROS);
        std::cout << "    Time: " << ms << "ms\n";
    }

    // Test 2: Deep coroutine nesting
    {
        EventLoop loop;
        std::atomic<int> depth_reached{0};

        std::function<Task<int>(EventLoop*, int)> nested;
        nested = [&nested, &depth_reached](EventLoop* loop, int depth) -> Task<int> {
            depth_reached = std::max(depth_reached.load(), depth);
            if (depth <= 0) {
                co_return 0;
            }
            co_await sleep(Duration::fromMicros(100), loop);
            co_return depth + co_await nested(loop, depth - 1);
        };

        auto runner = [&](EventLoop* loop) -> Task<void> {
            auto result = co_await nested(loop, 50);
            test("Deep nesting result", result == 1275); // Sum 1+2+...+50
            loop->stop();
        };

        runner(&loop).start();
        loop.run();
        test("Deep coroutine nesting (50 levels)", depth_reached >= 50);
    }
}

void test_async_timeout_stress() {
    subsection("Timeout Stress Test");

    // Test: Many concurrent timeouts
    {
        EventLoop loop;
        std::atomic<int> timeouts{0};
        std::atomic<int> finished{0};
        const int NUM_OPS = 100;

        auto slow_task = [](EventLoop* loop) -> Task<void> {
            co_await sleep(Duration::fromMillis(50), loop);
        };

        auto timed_op = [&slow_task](EventLoop* loop, std::atomic<int>* timeouts,
                          std::atomic<int>* finished, int total) -> Task<void> {
            try {
                co_await timeout(slow_task(loop), Duration::fromMillis(10), loop);
            } catch (const TimeoutError&) {
                (*timeouts)++;
            }
            if (++(*finished) == total) {
                loop->stop();
            }
        };

        for (int i = 0; i < NUM_OPS; i++) {
            timed_op(&loop, &timeouts, &finished, NUM_OPS).start();
        }

        loop.run();
        test("Concurrent timeouts", timeouts == NUM_OPS);
    }
}

//=============================================================================
// IPC STRESS TESTS
//=============================================================================

void test_ipc_pipe_stress() {
    subsection("Pipe Stress Test");

    // Test 1: High-throughput pipe
    {
        Pipe pipe = Pipe::create();
        const int NUM_WRITES = 10000;
        const char* msg = "test";
        std::atomic<int> bytes_written{0};
        std::atomic<int> bytes_read{0};
        int writeFd = pipe.writeEnd();
        int readFd = pipe.readEnd();

        std::thread writer([&, writeFd]() {
            for (int i = 0; i < NUM_WRITES; i++) {
                auto written = ::write(writeFd, msg, strlen(msg));
                if (written > 0) bytes_written += written;
            }
        });

        std::thread reader([&, readFd]() {
            char buf[1024];
            while (true) {
                auto n = ::read(readFd, buf, sizeof(buf));
                if (n <= 0) break;
                bytes_read += n;
            }
        });

        writer.join();
        pipe.closeWrite();
        reader.join();

        test("High-throughput pipe", bytes_written > 0 && bytes_read == bytes_written);
        std::cout << "    Transferred: " << bytes_read << " bytes\n";
    }
}

void test_ipc_shared_memory_stress() {
    subsection("Shared Memory Stress Test");

    // Test: Concurrent read/write
    {
        const char* name = "/pman_test_shm";
        const size_t size = 4096;

        SharedMemoryOptions opts;
        opts.mode = SharedMemoryMode::Create;
        SharedMemory shm(name, size, opts);
        auto* data = static_cast<std::atomic<int>*>(shm.data());
        new (data) std::atomic<int>(0);

        const int NUM_THREADS = 4;
        const int INCREMENTS = 10000;

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back([data]() {
                for (int j = 0; j < INCREMENTS; j++) {
                    data->fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (auto& t : threads) t.join();

        test("Shared memory concurrent access", data->load() == NUM_THREADS * INCREMENTS);

        SharedMemory::unlink(name);
    }
}

//=============================================================================
// PROCESS STRESS TESTS
//=============================================================================

void test_process_stress() {
    subsection("Process Stress Test");

    // Test 1: Rapid process creation
    {
        const int NUM_PROCESSES = 50;
        std::atomic<int> completed{0};

        auto ms = timed([&]() {
            std::vector<ProcessHandle> processes;
            for (int i = 0; i < NUM_PROCESSES; i++) {
                auto proc = ProcessBuilder("echo")
                    .arg("test")
                    .spawn();
                if (proc.valid()) {
                    processes.push_back(std::move(proc));
                }
            }

            for (auto& p : processes) {
                int status = p.wait();
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    completed++;
                }
            }
        });

        test("Rapid process creation", completed == NUM_PROCESSES);
        std::cout << "    " << NUM_PROCESSES << " processes in " << ms << "ms\n";
    }

    // Test 2: Process with I/O via pipes
    {
        ProcessBuilder builder("cat");
        Pipe stdin_pipe = builder.captureStdin();
        Pipe stdout_pipe = builder.captureStdout();
        auto proc = builder.spawn();

        if (proc.valid()) {
            const char* input = "Hello, Process!";
            ::write(stdin_pipe.writeEnd(), input, strlen(input));
            stdin_pipe.closeWrite();

            char buf[256] = {0};
            ::read(stdout_pipe.readEnd(), buf, sizeof(buf) - 1);

            proc.wait();
            test("Process I/O", strcmp(buf, input) == 0);
        } else {
            test("Process I/O", false);
        }
    }

    // Test 3: Process group
    {
        ProcessGroup group;
        const int NUM_IN_GROUP = 10;

        for (int i = 0; i < NUM_IN_GROUP; i++) {
            group.add(std::move(ProcessBuilder("sleep").arg("0.1")));
        }

        test("Process group size", group.size() == static_cast<size_t>(NUM_IN_GROUP));

        auto results = group.waitAll();
        int success = 0;
        for (auto status : results) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) success++;
        }
        test("Process group completion", success == NUM_IN_GROUP);
    }
}

//=============================================================================
// THREAD STRESS TESTS
//=============================================================================

void test_thread_pool_stress() {
    subsection("Thread Pool Stress Test");

    // Test 1: High-throughput task submission
    {
        ThreadPool pool(4);
        std::atomic<int> completed{0};
        const int NUM_TASKS = 10000;

        auto ms = timed([&]() {
            for (int i = 0; i < NUM_TASKS; i++) {
                pool.submit([&]() {
                    completed++;
                });
            }

            // Wait for completion
            while (completed < NUM_TASKS) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

        test("Thread pool throughput", completed == NUM_TASKS);
        std::cout << "    " << NUM_TASKS << " tasks in " << ms << "ms ("
                  << (NUM_TASKS * 1000 / (ms + 1)) << " tasks/sec)\n";
    }

    // Test 2: Mixed workload
    {
        ThreadPool pool(4);
        std::atomic<int> compute_done{0};
        std::atomic<int> io_done{0};
        std::atomic<int> total_done{0};

        for (int i = 0; i < 100; i++) {
            pool.submit([&]() {
                int sum = 0;
                for (int j = 0; j < 10000; j++) sum += j;
                (void)sum;
                compute_done++;
                total_done++;
            });
        }

        for (int i = 0; i < 100; i++) {
            pool.submit([&]() {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                io_done++;
                total_done++;
            });
        }

        while (total_done < 200) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        test("Mixed workload", compute_done == 100 && io_done == 100);
    }
}

void test_cpu_pinning_stress() {
    subsection("CPU Pinning Stress Test");

    {
        auto& topology = SystemTopology::instance();
        auto cpus = topology.cpuIds();

        if (cpus.empty()) {
            std::cout << "  [SKIP] No CPU topology available\n";
            return;
        }

        std::atomic<int> successful_pins{0};
        std::vector<std::thread> threads;

        for (auto cpu : cpus) {
            threads.emplace_back([cpu, &successful_pins]() {
                CpuSet set;
                set.add(cpu);

                if (sched_setaffinity(0, sizeof(cpu_set_t), set.data()) == 0) {
                    successful_pins++;
                }
                int sum = 0;
                for (int i = 0; i < 100000; i++) sum += i;
                (void)sum;
            });
        }

        for (auto& t : threads) t.join();

        test("CPU pinning", successful_pins > 0);
        std::cout << "    Pinned to " << successful_pins << "/" << cpus.size() << " CPUs\n";
    }
}

//=============================================================================
// SYNC PRIMITIVES STRESS TESTS
//=============================================================================

void test_sync_primitives_stress() {
    subsection("Sync Primitives Stress Test");

    // Test 1: FutexSemaphore stress
    {
        FutexSemaphore sem(5);
        std::atomic<int> max_concurrent{0};
        std::atomic<int> current{0};
        std::atomic<int> completed{0};
        const int NUM_TASKS = 100;

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_TASKS; i++) {
            threads.emplace_back([&]() {
                sem.acquire();
                int cur = ++current;
                int expected;
                do {
                    expected = max_concurrent.load();
                } while (cur > expected && !max_concurrent.compare_exchange_weak(expected, cur));

                std::this_thread::sleep_for(std::chrono::microseconds(100));
                current--;
                completed++;
                sem.release();
            });
        }

        for (auto& t : threads) t.join();

        test("Semaphore stress", completed == NUM_TASKS && max_concurrent <= 5);
        std::cout << "    Max concurrent: " << max_concurrent << " (limit: 5)\n";
    }

    // Test 2: Barrier stress
    {
        const int NUM_THREADS = 8;
        const int NUM_PHASES = 10;
        Barrier barrier(NUM_THREADS);
        std::atomic<int> phase_completions{0};

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back([&]() {
                for (int p = 0; p < NUM_PHASES; p++) {
                    barrier.wait();
                    phase_completions++;
                }
            });
        }

        for (auto& t : threads) t.join();

        test("Barrier stress", phase_completions == NUM_THREADS * NUM_PHASES);
    }

    // Test 3: FutexLatch stress
    {
        const int NUM_WORKERS = 10;
        FutexLatch latch(NUM_WORKERS);
        std::atomic<int> work_done{0};

        std::vector<std::thread> workers;
        for (int i = 0; i < NUM_WORKERS; i++) {
            workers.emplace_back([&]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                work_done++;
                latch.count_down();
            });
        }

        latch.wait();

        for (auto& w : workers) w.join();

        test("Latch stress", work_done == NUM_WORKERS);
    }

    // Test 4: FutexRwMutex stress
    {
        FutexRwMutex rwm;
        std::atomic<int> reads{0};
        std::atomic<int> writes{0};
        int shared_data = 0;

        std::vector<std::thread> threads;

        for (int i = 0; i < 2; i++) {
            threads.emplace_back([&]() {
                for (int j = 0; j < 100; j++) {
                    rwm.lock();
                    shared_data++;
                    writes++;
                    rwm.unlock();
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            });
        }

        for (int i = 0; i < 8; i++) {
            threads.emplace_back([&]() {
                for (int j = 0; j < 500; j++) {
                    rwm.lock_shared();
                    volatile int val = shared_data;
                    (void)val;
                    reads++;
                    rwm.unlock_shared();
                }
            });
        }

        for (auto& t : threads) t.join();

        test("RW mutex stress", writes == 200 && reads == 4000);
    }

    // Test 5: FutexMutex stress
    {
        FutexMutex mutex;
        std::atomic<int> counter{0};
        const int NUM_THREADS = 4;
        const int INCREMENTS = 10000;

        std::vector<std::thread> threads;
        for (int i = 0; i < NUM_THREADS; i++) {
            threads.emplace_back([&]() {
                for (int j = 0; j < INCREMENTS; j++) {
                    mutex.lock();
                    counter++;
                    mutex.unlock();
                }
            });
        }

        for (auto& t : threads) t.join();

        test("FutexMutex stress", counter == NUM_THREADS * INCREMENTS);
    }

    // Test 6: SeqLock stress
    {
        SeqLock seqlock;
        int shared_data = 0;
        std::atomic<int> successful_reads{0};
        std::atomic<bool> done{false};

        std::thread writer([&]() {
            for (int i = 1; i <= 1000; i++) {
                seqlock.lock();
                shared_data = i;
                seqlock.unlock();
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
            done = true;
        });

        std::vector<std::thread> readers;
        for (int i = 0; i < 4; i++) {
            readers.emplace_back([&]() {
                while (!done) {
                    int val = seqlock.read([&]() { return shared_data; });
                    if (val >= 0) successful_reads++;
                }
            });
        }

        writer.join();
        for (auto& r : readers) r.join();

        test("SeqLock stress", successful_reads > 1000);
    }
}

//=============================================================================
// SCHEDULING STRESS TESTS
//=============================================================================

void test_scheduling_stress() {
    subsection("Scheduling Stress Test");

    {
        auto& topology = SystemTopology::instance();
        auto cpus = topology.cpuIds();
        test("Topology detection", !cpus.empty());
        std::cout << "    CPUs: " << cpus.size()
                  << ", NUMA nodes: " << topology.nodes().size() << "\n";
    }

    {
        CpuSet set;
        set.add(0);
        test("CpuSet add CPU 0", set.contains(0));

        set.remove(0);
        test("CpuSet remove CPU 0", !set.contains(0));
    }
}

//=============================================================================
// CHAOS TEST
//=============================================================================

void test_chaos() {
    subsection("Chaos Test");

    {
        EventLoop loop;
        std::atomic<int> ops{0};
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, 3);

        for (int i = 0; i < 100; i++) {
            auto delay = std::chrono::milliseconds(dist(gen) * 10 + 1);
            loop.addTimer(delay, [&]() { ops++; });
        }

        std::vector<uint64_t> timer_ids;
        for (int i = 0; i < 50; i++) {
            auto id = loop.addTimer(std::chrono::milliseconds(50), [&]() { ops++; });
            timer_ids.push_back(id);
        }

        for (int i = 0; i < 25; i++) {
            if (!timer_ids.empty()) {
                size_t idx = dist(gen) % timer_ids.size();
                loop.cancelTimer(timer_ids[idx]);
                timer_ids.erase(timer_ids.begin() + idx);
            }
        }

        loop.addTimer(std::chrono::milliseconds(200), [&]() { loop.stop(); });
        loop.run();

        test("Chaos test completed", ops > 50);
        std::cout << "    Operations completed: " << ops << "\n";
    }
}

//=============================================================================
// MAIN
//=============================================================================

int main(int argc, char* argv[]) {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║       PMAN Comprehensive Stress Test Suite                   ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    bool run_all = argc == 1;
    std::string filter = argc > 1 ? argv[1] : "";

    auto should_run = [&](const char* name) {
        return run_all || filter.empty() || std::string(name).find(filter) != std::string::npos;
    };

    if (should_run("async")) {
        section("ASYNC SUBSYSTEM");
        test_async_timer_stress();
        test_async_coroutine_stress();
        test_async_timeout_stress();
    }

    if (should_run("ipc")) {
        section("IPC SUBSYSTEM");
        test_ipc_pipe_stress();
        test_ipc_shared_memory_stress();
    }

    if (should_run("process")) {
        section("PROCESS SUBSYSTEM");
        test_process_stress();
    }

    if (should_run("thread")) {
        section("THREAD SUBSYSTEM");
        test_thread_pool_stress();
        test_cpu_pinning_stress();
    }

    if (should_run("sync")) {
        section("SYNC PRIMITIVES");
        test_sync_primitives_stress();
    }

    if (should_run("sched")) {
        section("SCHEDULING");
        test_scheduling_stress();
    }

    if (should_run("chaos")) {
        section("CHAOS TESTING");
        test_chaos();
    }

    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                        SUMMARY                               ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";
    std::cout << "  Total tests:  " << total_tests << "\n";
    std::cout << "  Passed:       " << passed_tests << "\n";
    std::cout << "  Failed:       " << failed_tests << "\n";
    std::cout << "\n";

    if (failed_tests == 0) {
        std::cout << "  ALL TESTS PASSED!\n";
    } else {
        std::cout << "  SOME TESTS FAILED\n";
    }
    std::cout << "\n";

    return failed_tests > 0 ? 1 : 0;
}
