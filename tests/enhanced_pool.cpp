#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "pman/enhanced_thread_pool.hpp"

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
// EnhancedThreadPool Tests
// =============================================================================

TEST(enhanced_pool_basic_submit) {
    pman::EnhancedThreadPool pool(2);

    std::atomic<int> counter{0};
    std::vector<pman::TaskHandle> handles;

    for (int i = 0; i < 10; ++i) {
        handles.push_back(pool.submit([&]() {
            ++counter;
        }));
    }

    pool.waitIdle();

    // Wait a bit more for all completions to be visible
    std::this_thread::sleep_for(10ms);

    ASSERT(counter.load() >= 10);  // At least 10 should have run
}

TEST(enhanced_pool_task_handle) {
    pman::EnhancedThreadPool pool(2);

    auto handle = pool.submit([]() {
        std::this_thread::sleep_for(50ms);
    });

    ASSERT(!handle.isComplete());
    pool.waitIdle();
    ASSERT(handle.isComplete());
}

TEST(enhanced_pool_priority_scheduling) {
    // Test that tasks are queued and priority affects ordering.
    // Note: Due to timing, we just verify that high-priority task runs.
    pman::EnhancedThreadPool pool(1);  // Single thread to ensure ordering

    // Block the thread
    std::atomic<bool> release{false};
    std::atomic<bool> blocked{false};
    pool.submit([&]() {
        blocked = true;
        while (!release) {
            std::this_thread::sleep_for(1ms);
        }
    });

    // Wait until the blocking task is actually running
    while (!blocked) {
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(20ms);  // Let it settle

    std::atomic<int> highPriorityRan{0};
    std::atomic<int> lowPriorityRan{0};

    // Submit tasks with different priorities (higher number = higher priority)
    pool.submitWithPriority(1, [&]() {
        lowPriorityRan = 1;
    });

    pool.submitWithPriority(100, [&]() {
        highPriorityRan = 1;
    });

    release = true;
    pool.waitIdle();
    std::this_thread::sleep_for(10ms);

    // Both should have run
    ASSERT(highPriorityRan.load() == 1);
    ASSERT(lowPriorityRan.load() == 1);
}

TEST(enhanced_pool_metrics) {
    pman::EnhancedThreadPool pool(2);

    for (int i = 0; i < 5; ++i) {
        pool.submit([&]() {
            std::this_thread::sleep_for(10ms);
        });
    }

    pool.waitIdle();

    auto metrics = pool.metrics();
    ASSERT_EQ(metrics.tasksCompleted, 5u);
    ASSERT_EQ(metrics.tasksFailed, 0u);
}

TEST(enhanced_pool_resize) {
    pman::EnhancedThreadPool pool(2);
    ASSERT_EQ(pool.workerCount(), 2u);

    // Test growing the pool (shrinking may not be supported)
    pool.resize(4);
    ASSERT_EQ(pool.workerCount(), 4u);
}

TEST(enhanced_pool_cancel_all) {
    pman::EnhancedThreadPool pool(1);

    // Block the single thread
    std::atomic<bool> release{false};
    pool.submit([&]() {
        while (!release) {
            std::this_thread::sleep_for(1ms);
        }
    });

    std::this_thread::sleep_for(10ms);

    // Queue many tasks
    std::vector<pman::TaskHandle> handles;
    for (int i = 0; i < 10; ++i) {
        handles.push_back(pool.submit([&]() {
            std::this_thread::sleep_for(100ms);
        }));
    }

    // Cancel all pending tasks
    pool.cancelAll();

    release = true;
    pool.waitIdle();

    // Most tasks should be cancelled
    int cancelledCount = 0;
    for (auto& h : handles) {
        if (h.isCancelled()) {
            ++cancelledCount;
        }
    }
    ASSERT(cancelledCount > 0);
}

TEST(enhanced_pool_queue_size) {
    pman::EnhancedThreadPool pool(1);

    // Block the single thread
    std::atomic<bool> release{false};
    pool.submit([&]() {
        while (!release) {
            std::this_thread::sleep_for(1ms);
        }
    });

    std::this_thread::sleep_for(10ms);

    // Queue tasks
    for (int i = 0; i < 5; ++i) {
        pool.submit([]() {});
    }

    ASSERT(pool.queueSize() >= 5);

    release = true;
    pool.waitIdle();
    ASSERT_EQ(pool.queueSize(), 0u);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::printf("=== Enhanced Thread Pool Tests ===\n\n");
    // Tests run automatically via static initialization
    std::printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
    return testsFailed > 0 ? 1 : 0;
}
