/// @file async_timer_demo.cpp
/// @brief Comprehensive demo of async functions and timers in pman::async

#include "pman/async/channel.hpp"
#include "pman/async/event_loop.hpp"
#include "pman/async/interval.hpp"
#include "pman/async/sleep.hpp"
#include "pman/async/task.hpp"
#include "pman/async/timeout.hpp"

#include <chrono>
#include <iostream>

using namespace pman::async;
using namespace std::chrono;

// ============================================================================
// Demo 1: Basic Sleep
// ============================================================================

void demoBasicSleep() {
    std::cout << "\n=== Demo 1: Basic Sleep ===\n";

    EventLoop loop;

    auto task = [](EventLoop* loop) -> Task<void> {
        std::cout << "Starting countdown...\n";
        for (int i = 3; i > 0; --i) {
            std::cout << i << "...\n" << std::flush;
            co_await sleep(Duration::fromSeconds(1), loop);
        }
        std::cout << "Blast off!\n";
        loop->stop();
    };

    task(&loop).start();
    loop.run();

    std::cout << "\n";
}

// ============================================================================
// Demo 2: Repeating Interval
// ============================================================================

void demoInterval() {
    std::cout << "=== Demo 2: Repeating Interval ===\n";

    EventLoop loop;

    auto task = [](EventLoop* loop) -> Task<void> {
        std::cout << "Creating interval that ticks every 200ms...\n";

        auto interval = Interval::create(Duration::fromMillis(200), loop);

        int count = 0;
        while (auto tick = co_await interval.next()) {
            std::cout << "Tick #" << *tick << " at " << (*tick * 200) << "ms\n";

            if (++count >= 5) {
                std::cout << "Stopping interval after 5 ticks\n";
                interval.stop();
            }
        }

        std::cout << "Interval stopped\n";
        loop->stop();
    };

    task(&loop).start();
    loop.run();

    std::cout << "\n";
}

// ============================================================================
// Demo 3: Timeout
// ============================================================================

void demoTimeout() {
    std::cout << "=== Demo 3: Timeout ===\n";

    EventLoop loop;

    auto slowOp = [](int delay_ms, EventLoop* loop) -> Task<std::string> {
        co_await sleep(Duration::fromMillis(delay_ms), loop);
        co_return std::string("Completed after ") + std::to_string(delay_ms) + "ms";
    };

    auto task = [slowOp](EventLoop* loop) -> Task<void> {
        // Fast operation - should complete
        try {
            std::cout << "Fast operation (200ms timeout, 100ms task)...\n";
            auto result = co_await timeout(
                slowOp(100, loop),
                Duration::fromMillis(200),
                loop
            );
            std::cout << "✓ " << result << "\n";
        } catch (const TimeoutError&) {
            std::cout << "✗ Timed out!\n";
        }

        // Slow operation - should timeout
        try {
            std::cout << "\nSlow operation (200ms timeout, 500ms task)...\n";
            auto result = co_await timeout(
                slowOp(500, loop),
                Duration::fromMillis(200),
                loop
            );
            std::cout << "✓ " << result << "\n";
        } catch (const TimeoutError&) {
            std::cout << "✗ Timed out as expected!\n";
        }

        loop->stop();
    };

    task(&loop).start();
    loop.run();

    std::cout << "\n";
}

// ============================================================================
// Demo 4: Channels with Periodic Producer
// ============================================================================

void demoPeriodicChannel() {
    std::cout << "=== Demo 4: Periodic Channel ===\n";

    EventLoop loop;

    auto [tx, rx] = channel<int>(5);

    // Producer
    auto producer = [](Sender<int> tx, EventLoop* loop) -> Task<void> {
        for (int i = 1; i <= 5; ++i) {
            co_await tx.send(i);
            std::cout << "Produced: " << i << "\n";
            co_await sleep(Duration::fromMillis(150), loop);
        }
        tx.close();
    };

    // Consumer
    auto consumer = [](Receiver<int> rx, EventLoop* loop) -> Task<void> {
        while (auto value = co_await rx.recv()) {
            std::cout << "  Consumed: " << *value << "\n";
        }
        loop->stop();
    };

    producer(std::move(tx), &loop).start();
    consumer(std::move(rx), &loop).start();

    loop.run();

    std::cout << "\n";
}

// ============================================================================
// Demo 5: Task Composition
// ============================================================================

void demoTaskComposition() {
    std::cout << "=== Demo 5: Task Composition ===\n";

    EventLoop loop;

    auto step = [](int num, int value, int delay, EventLoop* loop) -> Task<int> {
        std::cout << "  Step " << num << ": processing " << value << "...\n";
        co_await sleep(Duration::fromMillis(delay), loop);
        int result = value * 2;
        std::cout << "  Step " << num << ": result = " << result << "\n";
        co_return result;
    };

    auto task = [step](EventLoop* loop) -> Task<void> {
        int value = 5;
        value = co_await step(1, value, 200, loop);
        value = co_await step(2, value, 150, loop);
        value = co_await step(3, value, 100, loop);
        std::cout << "\nFinal result: " << value << "\n";
        loop->stop();
    };

    task(&loop).start();
    loop.run();

    std::cout << "\n";
}

// ============================================================================
// Demo 6: Raw Timer Callbacks
// ============================================================================

void demoRawTimers() {
    std::cout << "=== Demo 6: Raw Timer Callbacks ===\n";

    EventLoop loop;

    int timersFired = 0;

    // Add multiple one-shot timers
    loop.addTimer(Duration::fromMillis(100).toChrono(), [&]() {
        std::cout << "Timer 1 fired at 100ms\n";
        timersFired++;
    });

    loop.addTimer(Duration::fromMillis(200).toChrono(), [&]() {
        std::cout << "Timer 2 fired at 200ms\n";
        timersFired++;
    });

    loop.addTimer(Duration::fromMillis(150).toChrono(), [&]() {
        std::cout << "Timer 3 fired at 150ms\n";
        timersFired++;
    });

    // Stop after all timers fire
    loop.addTimer(Duration::fromMillis(300).toChrono(), [&]() {
        std::cout << "\nAll " << timersFired << " timers completed\n";
        loop.stop();
    });

    loop.run();

    std::cout << "\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
    try {
        std::cout << "\n";
        std::cout << "╔════════════════════════════════════════════════╗\n";
        std::cout << "║   PMAN ASYNC TIMER DEMO                        ║\n";
        std::cout << "╚════════════════════════════════════════════════╝\n";

        demoBasicSleep();
        demoInterval();
        demoTimeout();
        demoPeriodicChannel();
        demoTaskComposition();
        demoRawTimers();

        std::cout << "╔════════════════════════════════════════════════╗\n";
        std::cout << "║   ALL DEMOS COMPLETED                          ║\n";
        std::cout << "╚════════════════════════════════════════════════╝\n\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
