/// @file async_sync_primitives_test.cpp
/// @brief Test async synchronization primitives: Mutex, Semaphore, RwLock

#include <iostream>
#include <atomic>
#include <vector>

#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/mutex.hpp"
#include "pman/async/semaphore.hpp"
#include "pman/async/rwlock.hpp"
#include "pman/async/sleep.hpp"

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

void test_mutex_basic() {
    std::cout << "\n--- Mutex Basic Test ---\n";

    EventLoop loop;
    Mutex mutex;
    std::atomic<int> counter{0};
    std::atomic<int> tasks_done{0};

    auto task = [&](int id) -> Task<void> {
        auto lock = co_await mutex.lock();
        int val = counter.load();
        co_await sleep(Duration::fromMillis(1), &loop);
        counter.store(val + 1);
        tasks_done++;
    };

    // Start 5 tasks that increment counter
    for (int i = 0; i < 5; i++) {
        task(i).start();
    }

    // Wait for completion
    loop.addTimer(std::chrono::milliseconds(100), [&]() {
        loop.stop();
    });

    loop.run();

    test("All tasks completed", tasks_done == 5);
    test("Counter incremented correctly", counter == 5);
}

void test_mutex_contention() {
    std::cout << "\n--- Mutex Contention Test ---\n";

    EventLoop loop;
    Mutex mutex;
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> tasks_done{0};

    auto task = [&]() -> Task<void> {
        auto lock = co_await mutex.lock();

        // Check no concurrent access
        int cur = ++concurrent;
        if (cur > max_concurrent) {
            max_concurrent = cur;
        }

        co_await sleep(Duration::fromMillis(5), &loop);

        --concurrent;
        tasks_done++;
    };

    // Start 10 tasks
    for (int i = 0; i < 10; i++) {
        task().start();
    }

    loop.addTimer(std::chrono::milliseconds(200), [&]() {
        loop.stop();
    });

    loop.run();

    test("All tasks completed", tasks_done == 10);
    test("No concurrent access", max_concurrent == 1);
}

void test_semaphore_basic() {
    std::cout << "\n--- Semaphore Basic Test ---\n";

    EventLoop loop;
    Semaphore sem(3);  // Allow 3 concurrent
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> tasks_done{0};

    auto task = [&]() -> Task<void> {
        co_await sem.acquire();

        int cur = ++concurrent;
        if (cur > max_concurrent) {
            max_concurrent = cur;
        }

        co_await sleep(Duration::fromMillis(10), &loop);

        --concurrent;
        sem.release();
        tasks_done++;
    };

    // Start 10 tasks, but only 3 can run concurrently
    for (int i = 0; i < 10; i++) {
        task().start();
    }

    loop.addTimer(std::chrono::milliseconds(300), [&]() {
        loop.stop();
    });

    loop.run();

    test("All tasks completed", tasks_done == 10);
    test("Respects concurrency limit", max_concurrent <= 3);
    test("Reached concurrency limit", max_concurrent == 3);
}

void test_semaphore_guard() {
    std::cout << "\n--- Semaphore RAII Guard Test ---\n";

    EventLoop loop;
    Semaphore sem(2);
    std::atomic<int> tasks_done{0};

    auto task = [&]() -> Task<void> {
        // Guard automatically releases on scope exit
        auto guard = co_await sem.acquireGuard();
        co_await sleep(Duration::fromMillis(10), &loop);
        tasks_done++;
    };

    for (int i = 0; i < 5; i++) {
        task().start();
    }

    loop.addTimer(std::chrono::milliseconds(150), [&]() {
        loop.stop();
    });

    loop.run();

    test("All tasks with guard completed", tasks_done == 5);
    test("Semaphore fully released", sem.available() == 2);
}

void test_rwlock_readers() {
    std::cout << "\n--- RwLock Multiple Readers Test ---\n";

    EventLoop loop;
    RwLock rwlock;
    std::atomic<int> concurrent_readers{0};
    std::atomic<int> max_concurrent_readers{0};
    std::atomic<int> tasks_done{0};

    auto reader = [&]() -> Task<void> {
        auto guard = co_await rwlock.readLock();

        int cur = ++concurrent_readers;
        if (cur > max_concurrent_readers) {
            max_concurrent_readers = cur;
        }

        co_await sleep(Duration::fromMillis(20), &loop);

        --concurrent_readers;
        tasks_done++;
    };

    // Start 5 readers - they should all run concurrently
    for (int i = 0; i < 5; i++) {
        reader().start();
    }

    loop.addTimer(std::chrono::milliseconds(100), [&]() {
        loop.stop();
    });

    loop.run();

    test("All readers completed", tasks_done == 5);
    test("Readers ran concurrently", max_concurrent_readers > 1);
}

void test_rwlock_writer_exclusive() {
    std::cout << "\n--- RwLock Writer Exclusivity Test ---\n";

    EventLoop loop;
    RwLock rwlock;
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> tasks_done{0};

    auto writer = [&]() -> Task<void> {
        auto guard = co_await rwlock.writeLock();

        int cur = ++concurrent;
        if (cur > max_concurrent) {
            max_concurrent = cur;
        }

        co_await sleep(Duration::fromMillis(10), &loop);

        --concurrent;
        tasks_done++;
    };

    // Start 5 writers - only one should run at a time
    for (int i = 0; i < 5; i++) {
        writer().start();
    }

    loop.addTimer(std::chrono::milliseconds(200), [&]() {
        loop.stop();
    });

    loop.run();

    test("All writers completed", tasks_done == 5);
    test("Writers ran exclusively", max_concurrent == 1);
}

void test_rwlock_mixed() {
    std::cout << "\n--- RwLock Mixed Readers/Writers Test ---\n";

    EventLoop loop;
    RwLock rwlock;
    std::atomic<int> readers_active{0};
    std::atomic<int> writers_active{0};
    std::atomic<bool> violation{false};
    std::atomic<int> tasks_done{0};

    auto reader = [&]() -> Task<void> {
        auto guard = co_await rwlock.readLock();

        readers_active++;
        // Check: readers OK, but no writers
        if (writers_active > 0) {
            violation = true;
        }

        co_await sleep(Duration::fromMillis(5), &loop);

        readers_active--;
        tasks_done++;
    };

    auto writer = [&]() -> Task<void> {
        auto guard = co_await rwlock.writeLock();

        writers_active++;
        // Check: only one writer, no readers
        if (writers_active > 1 || readers_active > 0) {
            violation = true;
        }

        co_await sleep(Duration::fromMillis(5), &loop);

        writers_active--;
        tasks_done++;
    };

    // Mix of readers and writers
    reader().start();
    reader().start();
    writer().start();
    reader().start();
    writer().start();
    reader().start();

    loop.addTimer(std::chrono::milliseconds(200), [&]() {
        loop.stop();
    });

    loop.run();

    test("All mixed tasks completed", tasks_done == 6);
    test("No lock violations", !violation);
}

void test_mutex_try_lock() {
    std::cout << "\n--- Mutex tryLock Test ---\n";

    EventLoop loop;
    Mutex mutex;
    bool first_acquired = false;
    bool second_failed = false;

    auto task1 = [&]() -> Task<void> {
        auto lock = co_await mutex.lock();
        first_acquired = true;

        co_await sleep(Duration::fromMillis(50), &loop);
    };

    auto task2 = [&]() -> Task<void> {
        co_await sleep(Duration::fromMillis(10), &loop);

        // Try to acquire while task1 holds it
        auto maybe_lock = mutex.tryLock();
        second_failed = !maybe_lock.has_value();

        loop.stop();
    };

    task1().start();
    task2().start();

    loop.run();

    test("First task acquired lock", first_acquired);
    test("Second tryLock failed correctly", second_failed);
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║        Async Sync Primitives Test Suite                      ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    try {
        test_mutex_basic();
        test_mutex_contention();
        test_mutex_try_lock();
        test_semaphore_basic();
        test_semaphore_guard();
        test_rwlock_readers();
        test_rwlock_writer_exclusive();
        test_rwlock_mixed();
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
