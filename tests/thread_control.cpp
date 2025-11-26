#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

// Note: cancellable_thread.hpp defines pman::ThreadCancellationToken
// and enhanced_thread_pool.hpp defines pman::CancellationToken.
// These are now separate types and both can be included if needed.

#include "pman/pausable_thread.hpp"
#include "pman/cancellable_thread.hpp"

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
// PausableThread Tests
// =============================================================================

TEST(pausable_thread_basic_execution) {
    std::atomic<int> counter{0};

    pman::PausableThread thread("test", [&](pman::PauseToken& token) {
        while (!token.stopRequested()) {
            ++counter;
            token.checkPause();
            std::this_thread::sleep_for(10ms);
        }
    });

    std::this_thread::sleep_for(50ms);
    thread.requestStop();
    thread.join();

    ASSERT(counter > 0);
}

TEST(pausable_thread_pause_resume) {
    std::atomic<int> counter{0};

    pman::PausableThread thread("test", [&](pman::PauseToken& token) {
        while (!token.stopRequested()) {
            ++counter;
            token.checkPause();
            std::this_thread::sleep_for(5ms);
        }
    });

    std::this_thread::sleep_for(30ms);
    int beforePause = counter.load();

    thread.pause();
    std::this_thread::sleep_for(50ms);
    int duringPause = counter.load();

    // Counter should not increase much during pause (maybe +1 for in-flight)
    ASSERT(duringPause - beforePause <= 1);

    thread.resume();
    std::this_thread::sleep_for(30ms);
    int afterResume = counter.load();

    ASSERT(afterResume > duringPause);

    thread.requestStop();
    thread.join();
}

TEST(pausable_thread_is_paused) {
    pman::PausableThread thread("test", [](pman::PauseToken& token) {
        while (!token.stopRequested()) {
            token.checkPause();
            std::this_thread::sleep_for(5ms);
        }
    });

    ASSERT(!thread.isPaused());
    thread.pause();
    std::this_thread::sleep_for(20ms);  // Give time for pause to take effect
    ASSERT(thread.isPaused());
    thread.resume();
    std::this_thread::sleep_for(20ms);  // Give time for resume
    ASSERT(!thread.isPaused());

    thread.requestStop();
    thread.join();
}

// =============================================================================
// CancellableThread Tests
// =============================================================================

TEST(cancellable_thread_basic) {
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};

    pman::CancellableThread thread("test", [&](pman::ThreadCancellationToken& token) {
        started = true;
        while (!token.isCancelled()) {
            std::this_thread::sleep_for(10ms);
        }
        finished = true;
    });

    std::this_thread::sleep_for(30ms);
    ASSERT(started);
    ASSERT(!finished);

    thread.cancel();
    thread.join();
    ASSERT(finished);
}

TEST(cancellable_thread_cleanup_handlers) {
    std::atomic<int> cleanupOrder{0};
    std::atomic<int> cleanup1{0};
    std::atomic<int> cleanup2{0};

    pman::CancellableThread thread("test", [&](pman::ThreadCancellationToken& token) {
        // Store the guards to keep them active
        auto guard1 = token.onCancel([&]() {
            cleanup1 = ++cleanupOrder;
        });
        auto guard2 = token.onCancel([&]() {
            cleanup2 = ++cleanupOrder;
        });

        while (!token.isCancelled()) {
            std::this_thread::sleep_for(10ms);
        }
    });

    std::this_thread::sleep_for(30ms);
    thread.cancel();
    thread.join();

    // Cleanup handlers run in LIFO order
    ASSERT_EQ(cleanup2.load(), 1);
    ASSERT_EQ(cleanup1.load(), 2);
}

TEST(cancellable_thread_throw_if_cancelled) {
    std::atomic<bool> exceptionThrown{false};

    pman::CancellableThread thread("test", [&](pman::ThreadCancellationToken& token) {
        try {
            while (true) {
                token.throwIfCancelled();
                std::this_thread::sleep_for(10ms);
            }
        } catch (const pman::CancelledException&) {
            exceptionThrown = true;
        }
    });

    std::this_thread::sleep_for(30ms);
    thread.cancel();
    thread.join();

    ASSERT(exceptionThrown);
}

TEST(cancellable_thread_polling_cancellation) {
    std::atomic<bool> exited{false};

    pman::CancellableThread thread("test", [&](pman::ThreadCancellationToken& token) {
        while (!token.isCancelled()) {
            std::this_thread::sleep_for(10ms);
        }
        exited = true;
    });

    std::this_thread::sleep_for(30ms);
    ASSERT(!exited);

    thread.cancel();
    thread.join();
    ASSERT(exited);
}

TEST(cancellable_thread_is_cancelled) {
    pman::CancellableThread thread("test", [](pman::ThreadCancellationToken& token) {
        while (!token.isCancelled()) {
            std::this_thread::sleep_for(10ms);
        }
    });

    ASSERT(!thread.isCancelled());
    thread.cancel();
    ASSERT(thread.isCancelled());
    thread.join();
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::printf("=== Thread Control Tests ===\n\n");
    // Tests run automatically via static initialization
    std::printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
    return testsFailed > 0 ? 1 : 0;
}
