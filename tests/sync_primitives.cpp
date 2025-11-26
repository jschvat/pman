#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

#include "pman/latch.hpp"
#include "pman/once.hpp"
#include "pman/recursive_mutex.hpp"
#include "pman/rw_mutex.hpp"
#include "pman/semaphore.hpp"

using namespace std::chrono_literals;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) \
    void test_##name(); \
    struct TestRunner_##name { \
        TestRunner_##name() { \
            std::printf("  Testing %s... ", #name); \
            std::fflush(stdout); \
            try { \
                test_##name(); \
                std::printf("PASSED\n"); \
                ++testsPassed; \
            } catch (const std::exception& e) { \
                std::printf("FAILED: %s\n", e.what()); \
                ++testsFailed; \
            } catch (...) { \
                std::printf("FAILED: unknown exception\n"); \
                ++testsFailed; \
            } \
        } \
    } testRunner_##name; \
    void test_##name()

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error("Assertion failed: " #cond); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            throw std::runtime_error("Assertion failed: " #a " == " #b); \
        } \
    } while (0)

// =============================================================================
// FutexSemaphore Tests
// =============================================================================

TEST(semaphore_basic_acquire_release) {
    pman::FutexSemaphore sem(2);

    ASSERT(sem.try_acquire());
    ASSERT(sem.try_acquire());
    ASSERT(!sem.try_acquire());  // Should fail, count is 0

    sem.release();
    ASSERT(sem.try_acquire());
    ASSERT(!sem.try_acquire());
}

TEST(semaphore_blocking_acquire) {
    pman::FutexSemaphore sem(0);
    std::atomic<bool> acquired{false};

    std::thread t([&]() {
        sem.acquire();
        acquired = true;
    });

    std::this_thread::sleep_for(50ms);
    ASSERT(!acquired);  // Should still be blocked

    sem.release();
    t.join();
    ASSERT(acquired);
}

TEST(semaphore_try_acquire_for) {
    pman::FutexSemaphore sem(0);

    auto start = std::chrono::steady_clock::now();
    bool result = sem.try_acquire_for(100ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT(!result);
    ASSERT(elapsed >= 90ms);  // Should have waited
}

TEST(semaphore_multiple_threads) {
    pman::FutexSemaphore sem(3);
    std::atomic<int> inCritical{0};
    std::atomic<int> maxConcurrent{0};

    auto worker = [&]() {
        for (int i = 0; i < 10; ++i) {
            sem.acquire();
            int current = ++inCritical;
            int expected = maxConcurrent.load();
            while (current > expected && !maxConcurrent.compare_exchange_weak(expected, current)) {}
            std::this_thread::sleep_for(1ms);
            --inCritical;
            sem.release();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    ASSERT(maxConcurrent <= 3);  // Never more than 3 concurrent
}

// =============================================================================
// FutexLatch Tests
// =============================================================================

TEST(latch_count_down_and_wait) {
    pman::FutexLatch latch(3);
    std::atomic<int> completed{0};

    auto worker = [&]() {
        std::this_thread::sleep_for(10ms);
        ++completed;
        latch.count_down();
    };

    std::thread t1(worker), t2(worker), t3(worker);

    latch.wait();
    ASSERT_EQ(completed.load(), 3);

    t1.join(); t2.join(); t3.join();
}

TEST(latch_arrive_and_wait) {
    pman::FutexLatch latch(2);
    std::atomic<int> stage{0};

    std::thread t([&]() {
        stage = 1;
        latch.arrive_and_wait();
        stage = 2;
    });

    std::this_thread::sleep_for(20ms);
    ASSERT_EQ(stage.load(), 1);

    latch.arrive_and_wait();  // Main thread arrives
    t.join();
    ASSERT_EQ(stage.load(), 2);
}

TEST(latch_try_wait) {
    pman::FutexLatch latch(1);

    ASSERT(!latch.try_wait());
    latch.count_down();
    ASSERT(latch.try_wait());
}

// =============================================================================
// FutexOnceFlag Tests
// =============================================================================

TEST(once_flag_single_execution) {
    pman::FutexOnceFlag flag;
    std::atomic<int> counter{0};

    auto tryCall = [&]() {
        flag.call_once([&]() {
            ++counter;
        });
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back(tryCall);
    }
    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(counter.load(), 1);
}

TEST(once_flag_concurrent_initialization) {
    pman::FutexOnceFlag flag;
    std::atomic<int> initCount{0};
    std::atomic<int> waiters{0};

    auto worker = [&]() {
        ++waiters;
        while (waiters < 5) {
            std::this_thread::yield();
        }
        flag.call_once([&]() {
            std::this_thread::sleep_for(50ms);
            ++initCount;
        });
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    ASSERT_EQ(initCount.load(), 1);
}

// =============================================================================
// FutexRwMutex Tests
// =============================================================================

TEST(rwmutex_exclusive_access) {
    pman::FutexRwMutex mutex;
    int value = 0;

    {
        pman::FutexWriteGuard guard(mutex);
        value = 42;
    }

    {
        pman::FutexReadGuard guard(mutex);
        ASSERT_EQ(value, 42);
    }
}

TEST(rwmutex_multiple_readers) {
    pman::FutexRwMutex mutex;
    std::atomic<int> readers{0};
    std::atomic<int> maxReaders{0};

    auto reader = [&]() {
        pman::FutexReadGuard guard(mutex);
        int current = ++readers;
        int expected = maxReaders.load();
        while (current > expected && !maxReaders.compare_exchange_weak(expected, current)) {}
        std::this_thread::sleep_for(20ms);
        --readers;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back(reader);
    }
    for (auto& t : threads) {
        t.join();
    }

    ASSERT(maxReaders >= 2);  // Multiple readers should overlap
}

TEST(rwmutex_writer_excludes_readers) {
    pman::FutexRwMutex mutex;
    std::atomic<bool> writerHolding{false};
    std::atomic<bool> readerSawWriter{false};

    std::thread writer([&]() {
        pman::FutexWriteGuard guard(mutex);
        writerHolding = true;
        std::this_thread::sleep_for(50ms);
        writerHolding = false;
    });

    std::this_thread::sleep_for(10ms);  // Let writer acquire

    std::thread reader([&]() {
        pman::FutexReadGuard guard(mutex);
        if (writerHolding) {
            readerSawWriter = true;
        }
    });

    writer.join();
    reader.join();

    ASSERT(!readerSawWriter);  // Reader should not see writer holding
}

TEST(rwmutex_try_lock) {
    pman::FutexRwMutex mutex;

    ASSERT(mutex.try_lock_shared());
    ASSERT(mutex.try_lock_shared());  // Multiple readers OK
    ASSERT(!mutex.try_lock());  // Writer blocked by readers
    mutex.unlock_shared();
    mutex.unlock_shared();

    ASSERT(mutex.try_lock());
    ASSERT(!mutex.try_lock_shared());  // Reader blocked by writer
    ASSERT(!mutex.try_lock());  // Writer blocked by writer
    mutex.unlock();
}

TEST(rwmutex_upgrade_downgrade) {
    pman::FutexRwMutex mutex;
    int value = 0;

    mutex.lock_shared();
    int readValue = value;

    // Upgrade to write (must unlock shared first, then try_upgrade won't work here)
    mutex.unlock_shared();
    mutex.lock();
    value = readValue + 1;

    // Downgrade to read
    mutex.downgrade();
    ASSERT_EQ(value, 1);
    mutex.unlock_shared();
}

// =============================================================================
// FutexRecursiveMutex Tests
// =============================================================================

TEST(recursive_mutex_single_lock) {
    pman::FutexRecursiveMutex mutex;
    int value = 0;

    {
        pman::FutexRecursiveLockGuard guard(mutex);
        value = 42;
    }

    ASSERT_EQ(value, 42);
}

TEST(recursive_mutex_recursive_lock) {
    pman::FutexRecursiveMutex mutex;
    int depth = 0;

    std::function<void(int)> recurse = [&](int n) {
        pman::FutexRecursiveLockGuard guard(mutex);
        ++depth;
        if (n > 0) {
            recurse(n - 1);
        }
    };

    recurse(5);
    ASSERT_EQ(depth, 6);  // 0 through 5
}

TEST(recursive_mutex_different_threads) {
    pman::FutexRecursiveMutex mutex;
    std::atomic<bool> t1Holding{false};
    std::atomic<bool> t2Blocked{false};

    std::thread t1([&]() {
        pman::FutexRecursiveLockGuard guard(mutex);
        t1Holding = true;
        while (!t2Blocked) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(50ms);
        t1Holding = false;
    });

    std::this_thread::sleep_for(10ms);  // Let t1 acquire

    std::thread t2([&]() {
        t2Blocked = true;
        pman::FutexRecursiveLockGuard guard(mutex);
        ASSERT(!t1Holding);  // t1 should have released
    });

    t1.join();
    t2.join();
}

TEST(recursive_mutex_try_lock) {
    pman::FutexRecursiveMutex mutex;

    ASSERT(mutex.try_lock());
    ASSERT(mutex.try_lock());  // Recursive OK
    mutex.unlock();
    mutex.unlock();

    // Lock from another thread should fail
    mutex.lock();
    std::atomic<bool> otherGotLock{false};
    std::thread t([&]() {
        otherGotLock = mutex.try_lock();
    });
    t.join();
    ASSERT(!otherGotLock);
    mutex.unlock();
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::printf("=== Synchronization Primitives Tests ===\n\n");
    // Tests run automatically via static initialization
    std::printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
    return testsFailed > 0 ? 1 : 0;
}
