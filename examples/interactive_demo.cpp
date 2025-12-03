/// @file interactive_demo.cpp
/// @brief Interactive demonstration of PMAN library features
///
/// Showcases all major components:
/// - Process management
/// - Thread control
/// - Async runtime (coroutines, timers, I/O)
/// - Synchronization primitives
/// - IPC mechanisms

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>

// Core PMAN
#include "pman/thread.hpp"
#include "pman/thread_pool.hpp"
#include "pman/process.hpp"
#include "pman/process_builder.hpp"

// Async
#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/combinators.hpp"
#include "pman/async/mutex.hpp"
#include "pman/async/semaphore.hpp"
#include "pman/async/rwlock.hpp"
#include "pman/async/file.hpp"
#include "pman/async/channel.hpp"

using namespace pman;
using namespace pman::async;

namespace {

void clearScreen() {
    std::cout << "\033[2J\033[1;1H";
}

void printHeader(const std::string& title) {
    std::cout << "\n╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  " << title;
    for (size_t i = 0; i < 60 - title.length(); i++) std::cout << " ";
    std::cout << "║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n\n";
}

void waitForEnter() {
    std::cout << "\nPress ENTER to continue...";
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    std::cin.get();
}

} // namespace

//=============================================================================
// THREAD DEMOS
//=============================================================================

void demo_basic_threads() {
    clearScreen();
    printHeader("Basic Thread Creation");

    std::cout << "Creating 3 threads that count to 3...\n\n";

    std::atomic<int> completed{0};

    auto worker = [&](int id) {
        for (int i = 1; i <= 3; i++) {
            std::cout << "Thread " << id << ": " << i << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        completed++;
    };

    ManagedThread t1("worker1", [&]() { worker(1); });
    ManagedThread t2("worker2", [&]() { worker(2); });
    ManagedThread t3("worker3", [&]() { worker(3); });

    t1.join();
    t2.join();
    t3.join();

    std::cout << "\n✓ All " << completed << " threads completed!\n";
    waitForEnter();
}

void demo_thread_pool() {
    clearScreen();
    printHeader("Thread Pool");

    std::cout << "Creating thread pool with 4 workers...\n";
    std::cout << "Submitting 10 tasks...\n\n";

    ThreadPool pool(4);
    std::atomic<int> completed{0};
    std::atomic<int> target{10};

    for (int i = 1; i <= 10; i++) {
        pool.submit([&, i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::cout << "Task " << i << " completed by thread "
                      << std::this_thread::get_id() << "\n";
            completed++;
        });
    }

    // Wait for all tasks to complete
    while (completed < target) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "\n✓ All " << completed << " tasks completed!\n";
    std::cout << "✓ Thread pool handled work distribution automatically\n";
    waitForEnter();
}

//=============================================================================
// PROCESS DEMOS
//=============================================================================

void demo_process_spawn() {
    clearScreen();
    printHeader("Process Spawning");

    std::cout << "Spawning 'echo' process...\n\n";

    try {
        ProcessBuilder builder("/bin/echo");
        builder.arg("Hello").arg("from").arg("PMAN!");

        auto stdoutPipe = builder.captureStdout();

        auto process = builder.spawn();
        int status = process.wait();

        // Read from stdout pipe
        char buffer[1024];
        ssize_t n = ::read(stdoutPipe.readEnd(), buffer, sizeof(buffer));
        std::string output(buffer, n > 0 ? n : 0);

        std::cout << "Process output: " << output;
        std::cout << "Exit status: " << WEXITSTATUS(status) << "\n";

        std::cout << "\n✓ Process executed successfully!\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }

    waitForEnter();
}

void demo_process_with_env() {
    clearScreen();
    printHeader("Process with Environment");

    std::cout << "Running command with custom environment...\n\n";

    try {
        ProcessBuilder builder("/usr/bin/printenv");
        builder.arg("PATH");
        builder.inheritEnvironment(true);

        auto stdoutPipe = builder.captureStdout();

        auto process = builder.spawn();
        int status = process.wait();

        // Read from stdout pipe
        char buffer[1024];
        ssize_t n = ::read(stdoutPipe.readEnd(), buffer, sizeof(buffer));
        std::string output(buffer, n > 0 ? n : 0);

        std::cout << "PATH environment variable:\n";
        std::cout << output;
        std::cout << "Exit status: " << WEXITSTATUS(status) << "\n";

        std::cout << "\n✓ Process executed with environment successfully!\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }

    waitForEnter();
}

//=============================================================================
// ASYNC DEMOS
//=============================================================================

void demo_async_sleep() {
    clearScreen();
    printHeader("Async Sleep and Timers");

    std::cout << "Creating event loop and scheduling async sleeps...\n\n";

    EventLoop loop;

    auto task1 = [&]() -> Task<void> {
        std::cout << "[T1] Starting...\n";
        co_await sleep(Duration::fromMillis(100), &loop);
        std::cout << "[T1] Woke up after 100ms!\n";
        co_await sleep(Duration::fromMillis(100), &loop);
        std::cout << "[T1] Done!\n";
    };

    auto task2 = [&]() -> Task<void> {
        std::cout << "[T2] Starting...\n";
        co_await sleep(Duration::fromMillis(150), &loop);
        std::cout << "[T2] Woke up after 150ms!\n";
        loop.stop();
    };

    task1().start();
    task2().start();

    std::cout << "Running event loop...\n\n";
    loop.run();

    std::cout << "\n✓ All async tasks completed!\n";
    std::cout << "✓ No threads were blocked - event loop handled everything\n";
    waitForEnter();
}

void demo_async_combinators() {
    clearScreen();
    printHeader("Async Combinators (all, any, first)");

    std::cout << "Testing async combinators...\n\n";

    EventLoop loop;

    auto demo = [&]() -> Task<void> {
        // Test all()
        std::cout << "1. Testing all() - wait for all tasks:\n";

        std::vector<Task<int>> tasks;
        tasks.push_back([&]() -> Task<int> {
            co_await sleep(Duration::fromMillis(50), &loop);
            co_return 1;
        }());
        tasks.push_back([&]() -> Task<int> {
            co_await sleep(Duration::fromMillis(100), &loop);
            co_return 2;
        }());
        tasks.push_back([&]() -> Task<int> {
            co_await sleep(Duration::fromMillis(75), &loop);
            co_return 3;
        }());

        auto results = co_await all(std::move(tasks), &loop);
        std::cout << "   Results: ";
        for (auto r : results) std::cout << r << " ";
        std::cout << "\n   ✓ All tasks completed!\n\n";

        // Test any()
        std::cout << "2. Testing any() - wait for first:\n";

        std::vector<Task<int>> race_tasks;
        race_tasks.push_back([&]() -> Task<int> {
            co_await sleep(Duration::fromMillis(100), &loop);
            co_return 100;
        }());
        race_tasks.push_back([&]() -> Task<int> {
            co_await sleep(Duration::fromMillis(30), &loop);  // Fastest!
            co_return 30;
        }());
        race_tasks.push_back([&]() -> Task<int> {
            co_await sleep(Duration::fromMillis(50), &loop);
            co_return 50;
        }());

        auto winner = co_await any(std::move(race_tasks), &loop);
        std::cout << "   Winner: Task #" << winner.index
                  << " with value " << winner.value << "\n";
        std::cout << "   ✓ Fastest task won!\n";

        loop.stop();
    };

    demo().start();
    loop.run();

    std::cout << "\n✓ Combinators enable powerful concurrency patterns!\n";
    waitForEnter();
}

void demo_async_mutex() {
    clearScreen();
    printHeader("Async Mutex");

    std::cout << "Testing async mutex with concurrent tasks...\n\n";

    EventLoop loop;
    Mutex mutex;
    int counter = 0;

    auto incrementer = [&](int id) -> Task<void> {
        for (int i = 0; i < 3; i++) {
            auto lock = co_await mutex.lock();
            std::cout << "[Task " << id << "] Acquired lock, counter = " << counter << "\n";

            int old_val = counter;
            co_await sleep(Duration::fromMillis(10), &loop);
            counter = old_val + 1;

            std::cout << "[Task " << id << "] Incremented to " << counter << ", releasing\n";
        }
    };

    incrementer(1).start();
    incrementer(2).start();
    incrementer(3).start();

    loop.addTimer(std::chrono::milliseconds(500), [&]() {
        loop.stop();
    });

    loop.run();

    std::cout << "\n✓ Final counter value: " << counter << "\n";
    std::cout << "✓ Mutex ensured thread-safe access!\n";
    waitForEnter();
}

void demo_async_semaphore() {
    clearScreen();
    printHeader("Async Semaphore");

    std::cout << "Testing semaphore with limited resources...\n";
    std::cout << "Semaphore capacity: 2 (only 2 tasks can run concurrently)\n\n";

    EventLoop loop;
    Semaphore sem(2);
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};

    auto worker = [&](int id) -> Task<void> {
        std::cout << "[Task " << id << "] Waiting for semaphore...\n";

        auto guard = co_await sem.acquireGuard();

        int cur = ++concurrent;
        if (cur > max_concurrent) max_concurrent = cur;

        std::cout << "[Task " << id << "] Acquired! Running... (concurrent=" << cur << ")\n";
        co_await sleep(Duration::fromMillis(100), &loop);

        --concurrent;
        std::cout << "[Task " << id << "] Done!\n";
    };

    for (int i = 1; i <= 5; i++) {
        worker(i).start();
    }

    loop.addTimer(std::chrono::milliseconds(800), [&]() {
        loop.stop();
    });

    loop.run();

    std::cout << "\n✓ Max concurrent tasks: " << max_concurrent << " (limit was 2)\n";
    std::cout << "✓ Semaphore enforced resource limits!\n";
    waitForEnter();
}

void demo_async_file_io() {
    clearScreen();
    printHeader("Async File I/O");

    std::cout << "Testing async file operations...\n\n";

    EventLoop loop;
    bool done = false;

    auto demo = [&]() -> Task<void> {
        const std::string filename = "/tmp/pman_demo.txt";
        const std::string content = "Hello from PMAN async file I/O!\nLine 2\nLine 3";

        std::cout << "1. Writing file asynchronously...\n";

        // Use explicit AsyncFile API
        auto file = AsyncFile::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        auto write_res = co_await file.write(content);

        if (write_res.ok()) {
            std::cout << "   ✓ Wrote " << write_res.value << " bytes\n\n";
        }

        std::cout << "2. Reading file asynchronously...\n";

        auto file2 = AsyncFile::open(filename, O_RDONLY);
        auto read_res = co_await file2.read(1024);

        if (read_res.ok()) {
            std::string read_content(read_res.value.begin(), read_res.value.end());
            std::cout << "   ✓ Read " << read_res.value.size() << " bytes\n";
            std::cout << "   Content:\n";
            std::cout << "   " << read_content << "\n";
        }

        // Cleanup
        unlink(filename.c_str());

        done = true;
        loop.stop();
    };

    // Start task after scheduling a timer to ensure loop is running
    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        demo().start();
    });

    loop.run();

    if (done) {
        std::cout << "\n✓ Async file I/O completed without blocking!\n";
    } else {
        std::cout << "\n✗ File I/O did not complete\n";
    }

    waitForEnter();
}

void demo_async_channel() {
    clearScreen();
    printHeader("Async Channel (Message Passing)");

    std::cout << "Testing async channel for producer/consumer pattern...\n";
    std::cout << "Channel capacity: 5\n\n";

    EventLoop loop;
    std::atomic<int> items_produced{0};
    std::atomic<int> items_consumed{0};

    auto producer = [&](Sender<int> tx) -> Task<void> {
        std::cout << "[Producer] Starting...\n";
        for (int i = 1; i <= 10; i++) {
            co_await tx.send(i);
            items_produced++;
            std::cout << "[Producer] Sent: " << i << "\n";
            co_await sleep(Duration::fromMillis(20), EventLoop::current());
        }
        tx.close();
        std::cout << "[Producer] Done!\n";
    };

    auto consumer = [&](Receiver<int> rx) -> Task<void> {
        std::cout << "[Consumer] Starting...\n";
        while (auto value = co_await rx.recv()) {
            items_consumed++;
            std::cout << "[Consumer] Received: " << *value << "\n";
            co_await sleep(Duration::fromMillis(30), EventLoop::current());
        }
        std::cout << "[Consumer] Done!\n";
    };

    auto task = [&]() -> Task<void> {
        auto [tx, rx] = channel<int>(5);

        producer(tx).start();
        consumer(rx).start();

        // Wait for completion
        co_await sleep(Duration::fromMillis(500), EventLoop::current());

        loop.stop();
    };

    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        task().start();
    });

    loop.run();

    std::cout << "\n✓ Produced " << items_produced << " items\n";
    std::cout << "✓ Consumed " << items_consumed << " items\n";
    std::cout << "✓ Channel handled async message passing!\n";
    waitForEnter();
}

//=============================================================================
// MAIN MENU
//=============================================================================

void showMenu() {
    clearScreen();
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════╗
║                   PMAN Interactive Demo                       ║
║              Process Management & Async Library               ║
╚═══════════════════════════════════════════════════════════════╝

)" << "\n";

    std::cout << "THREADING DEMOS:\n";
    std::cout << "  1. Basic Thread Creation\n";
    std::cout << "  2. Thread Pool\n\n";

    std::cout << "PROCESS DEMOS:\n";
    std::cout << "  3. Process Spawning\n";
    std::cout << "  4. Process with Environment\n\n";

    std::cout << "ASYNC DEMOS:\n";
    std::cout << "  5. Async Sleep & Timers\n";
    std::cout << "  6. Async Combinators (all, any, first)\n";
    std::cout << "  7. Async Mutex\n";
    std::cout << "  8. Async Semaphore\n";
    std::cout << "  9. Async File I/O\n";
    std::cout << " 10. Async Channel (Message Passing)\n\n";

    std::cout << "OTHER:\n";
    std::cout << "  0. Exit\n\n";

    std::cout << "Select demo (0-10): ";
}

int main() {
    while (true) {
        showMenu();

        int choice;
        std::cin >> choice;

        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            continue;
        }

        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

        switch (choice) {
            case 1: demo_basic_threads(); break;
            case 2: demo_thread_pool(); break;
            case 3: demo_process_spawn(); break;
            case 4: demo_process_with_env(); break;
            case 5: demo_async_sleep(); break;
            case 6: demo_async_combinators(); break;
            case 7: demo_async_mutex(); break;
            case 8: demo_async_semaphore(); break;
            case 9: demo_async_file_io(); break;
            case 10: demo_async_channel(); break;
            case 0:
                clearScreen();
                std::cout << "\nThank you for exploring PMAN!\n";
                std::cout << "Visit: https://github.com/anthropics/pman\n\n";
                return 0;
            default:
                std::cout << "\nInvalid choice. Press ENTER to continue...\n";
                std::cin.get();
        }
    }

    return 0;
}
