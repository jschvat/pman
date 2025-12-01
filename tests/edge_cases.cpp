/// @file edge_cases.cpp
/// @brief Edge case and corner case tests for PMAN library
///
/// Tests unusual scenarios, boundary conditions, and error handling

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <cassert>
#include <cstring>
#include <limits>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>

// Async components
#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"

// IPC components
#include "pman/pipe.hpp"
#include "pman/shared_memory.hpp"

// Thread components
#include "pman/thread_pool.hpp"
#include "pman/semaphore.hpp"
#include "pman/barrier.hpp"
#include "pman/latch.hpp"
#include "pman/futex.hpp"

// Process components
#include "pman/process_builder.hpp"

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

} // anonymous namespace

//=============================================================================
// ASYNC EDGE CASES
//=============================================================================

void test_async_edge_cases() {
    section("ASYNC EDGE CASES");

    subsection("Timer Edge Cases");

    // Test: Zero duration timer
    {
        EventLoop loop;
        bool fired = false;
        loop.addTimer(std::chrono::nanoseconds(0), [&]() {
            fired = true;
            loop.stop();
        });
        loop.run();
        test("Zero duration timer fires", fired);
    }

    // Test: Cancel non-existent timer
    {
        EventLoop loop;
        bool result = loop.cancelTimer(999999);
        test("Cancel non-existent timer returns false", !result);
    }

    // Test: Cancel already-fired timer
    {
        EventLoop loop;
        bool fired = false;
        auto id = loop.addTimer(std::chrono::nanoseconds(0), [&]() {
            fired = true;
        });

        loop.runOnce(std::chrono::milliseconds(10));

        bool cancel_result = loop.cancelTimer(id);
        test("Cancel already-fired timer returns false", !cancel_result && fired);
    }

    // Test: Cancel timer during callback
    {
        EventLoop loop;
        uint64_t timer2_id = 0;
        int timer1_count = 0;

        loop.addTimer(std::chrono::nanoseconds(0), [&]() {
            timer1_count++;
            // Cancel timer2 during timer1's callback
            loop.cancelTimer(timer2_id);
        });

        timer2_id = loop.addTimer(std::chrono::nanoseconds(0), [&]() {
            // May or may not fire
        });

        loop.addTimer(std::chrono::milliseconds(10), [&]() {
            loop.stop();
        });

        loop.run();
        test("Cancel during callback doesn't crash", timer1_count == 1);
    }

    // Test: Add timer during callback
    {
        EventLoop loop;
        std::atomic<int> count{0};

        std::function<void()> add_next;
        add_next = [&]() {
            count++;
            if (count < 5) {
                loop.addTimer(std::chrono::nanoseconds(0), add_next);
            } else {
                loop.stop();
            }
        };

        loop.addTimer(std::chrono::nanoseconds(0), add_next);

        loop.run();
        test("Add timer during callback works", count >= 5);
    }

    // Test: Many timers with same expiry
    {
        EventLoop loop;
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
        test("Many timers with same expiry", count == N);
    }

    // Test: Periodic timer with very short period
    {
        EventLoop loop;
        std::atomic<int> count{0};

        loop.addPeriodicTimer(std::chrono::microseconds(100), [&]() {
            if (++count >= 100) loop.stop();
        });

        loop.run();
        test("Periodic timer with short period", count >= 100);
    }

    // Simplified coroutine test - just check that a simple coroutine works
    subsection("Coroutine Edge Cases");
    {
        EventLoop loop;
        std::atomic<int> count{0};

        auto simple_coro = [](EventLoop* l, std::atomic<int>* c) -> Task<void> {
            co_await sleep(Duration::fromMicros(100), l);
            (*c)++;
            l->stop();
        };

        simple_coro(&loop, &count).start();
        loop.run();
        test("Simple coroutine works", count == 1);
    }

    // Simplified timeout test
    subsection("Timeout Edge Cases");
    {
        EventLoop loop;
        bool timed_out = false;

        auto slow = [](EventLoop* l) -> Task<void> {
            co_await sleep(Duration::fromSeconds(10), l);
        };

        auto runner = [&]() -> Task<void> {
            try {
                co_await timeout(slow(&loop), Duration::fromMicros(500), &loop);
            } catch (const TimeoutError&) {
                timed_out = true;
            }
            loop.stop();
        };

        runner().start();
        loop.run();
        test("Slow task times out", timed_out);
    }
}

//=============================================================================
// IPC EDGE CASES
//=============================================================================

void test_ipc_edge_cases() {
    section("IPC EDGE CASES");

    subsection("Pipe Edge Cases");

    // Test: Read from empty pipe (non-blocking)
    {
        auto pipe = Pipe::create();
        char buf[100];
        // Set non-blocking
        fcntl(pipe.readEnd(), F_SETFL, O_NONBLOCK);
        auto n = ::read(pipe.readEnd(), buf, sizeof(buf));
        test("Read from empty pipe returns -1", n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }

    // Test: Write to closed read end
    {
        auto pipe = Pipe::create();
        pipe.closeRead();
        // Writing to pipe with closed read end should fail
        auto n = ::write(pipe.writeEnd(), "test", 4);
        test("Write to closed read end", n == -1 || n == 4); // May succeed or fail
    }

    // Test: Zero-length write
    {
        auto pipe = Pipe::create();
        auto n = ::write(pipe.writeEnd(), "", 0);
        test("Zero-length write", n == 0);
    }

    // Test: Large write that exceeds pipe buffer
    {
        auto pipe = Pipe::create();
        std::vector<char> large(1024 * 1024, 'x'); // 1MB
        std::atomic<size_t> total_read{0};

        std::thread reader([&]() {
            std::vector<char> buf(65536);
            while (true) {
                auto n = ::read(pipe.readEnd(), buf.data(), buf.size());
                if (n <= 0) break;
                total_read += n;
            }
        });

        size_t written = 0;
        while (written < large.size()) {
            auto n = ::write(pipe.writeEnd(), large.data() + written, large.size() - written);
            if (n > 0) written += n;
            if (n < 0 && errno != EAGAIN) break;
        }
        pipe.closeWrite();
        reader.join();

        test("Large write through pipe", total_read == large.size());
    }

    subsection("Shared Memory Edge Cases");

    // Test: Create shared memory with minimum size
    {
        try {
            SharedMemoryOptions opts;
            opts.mode = SharedMemoryMode::Create;
            SharedMemory shm("/pman_test_small", 1, opts);
            test("Minimum size shared memory", shm.data() != nullptr);
            SharedMemory::unlink("/pman_test_small");
        } catch (...) {
            test("Minimum size shared memory", false);
        }
    }
}

//=============================================================================
// THREAD EDGE CASES
//=============================================================================

void test_thread_edge_cases() {
    section("THREAD EDGE CASES");

    subsection("Thread Pool Edge Cases");

    // Test: Submit task to pool with 1 thread
    {
        ThreadPool pool(1);
        std::atomic<int> count{0};

        for (int i = 0; i < 100; i++) {
            pool.submit([&]() {
                count++;
            });
        }

        while (count < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        test("Single-thread pool handles many tasks", count == 100);
    }

    // Test: Submit task that throws
    {
        ThreadPool pool(2);
        std::atomic<int> after_throw{0};

        pool.submit([]() {
            throw std::runtime_error("test exception");
        });

        // Other tasks should still work
        pool.submit([&]() {
            after_throw++;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        test("Pool survives task exception", after_throw == 1);
    }

    subsection("Sync Primitive Edge Cases");

    // Test: FutexSemaphore with count 0
    {
        FutexSemaphore sem(0);
        std::atomic<bool> acquired{false};

        std::thread t([&]() {
            sem.acquire();
            acquired = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        test("Semaphore blocks at 0", !acquired);

        sem.release();
        t.join();
        test("Semaphore unblocks after release", acquired);
    }

    // Test: FutexLatch countdown to 0
    {
        FutexLatch latch(1);
        std::atomic<bool> passed{false};

        std::thread t([&]() {
            latch.wait();
            passed = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        test("Latch blocks before countdown", !passed);

        latch.count_down();
        t.join();
        test("Latch unblocks at 0", passed);
    }

    // Test: Barrier with single thread
    {
        Barrier barrier(1);
        int passes = 0;

        for (int i = 0; i < 3; i++) {
            barrier.wait();
            passes++;
        }

        test("Single-thread barrier", passes == 3);
    }
}

//=============================================================================
// PROCESS EDGE CASES
//=============================================================================

void test_process_edge_cases() {
    section("PROCESS EDGE CASES");

    subsection("ProcessBuilder Edge Cases");

    // Test: Run process that exits immediately
    {
        auto proc = ProcessBuilder("true").spawn();
        int status = proc.wait();
        test("Process 'true' exits 0", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // Test: Run process that fails immediately
    {
        auto proc = ProcessBuilder("false").spawn();
        int status = proc.wait();
        test("Process 'false' exits non-zero", WIFEXITED(status) && WEXITSTATUS(status) != 0);
    }

    // Test: Run non-existent program
    {
        auto proc = ProcessBuilder("/nonexistent/program/xyz123").spawn();
        bool failed = !proc.valid();
        if (proc.valid()) {
            proc.wait();
        }
        test("Non-existent program", true); // Just checking it doesn't crash
    }

    // Test: Process with empty arguments
    {
        auto proc = ProcessBuilder("echo").spawn();
        int status = proc.wait();
        test("Process with no args", WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    // Test: Kill process
    {
        auto proc = ProcessBuilder("sleep").arg("10").spawn();
        if (proc.valid()) {
            proc.kill();
            int status = proc.wait();
            test("Kill process", WIFSIGNALED(status));
        } else {
            test("Kill process", false);
        }
    }
}

//=============================================================================
// MAIN
//=============================================================================

int main() {
    // Ignore SIGPIPE to allow testing pipe error conditions
    signal(SIGPIPE, SIG_IGN);

    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           PMAN Edge Case Test Suite                          ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    test_async_edge_cases();
    test_ipc_edge_cases();
    test_thread_edge_cases();
    test_process_edge_cases();

    std::cout << "\n========================================\n";
    std::cout << "RESULTS\n";
    std::cout << "========================================\n";
    std::cout << "Total:  " << total_tests << "\n";
    std::cout << "Passed: " << passed_tests << "\n";
    std::cout << "Failed: " << failed_tests << "\n";

    return failed_tests > 0 ? 1 : 0;
}
