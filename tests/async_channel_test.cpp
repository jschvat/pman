/// @file async_channel_test.cpp
/// @brief Test async channel (MPMC message passing)

#include <iostream>
#include <atomic>
#include <vector>

#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/channel.hpp"
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

void test_basic_send_recv() {
    std::cout << "\n--- Basic Send/Recv Test ---\n";

    EventLoop loop;
    bool sent = false;
    bool received = false;
    int received_value = 0;

    auto task = [&]() -> Task<void> {
        auto [tx, rx] = channel<int>(1);

        // Send
        sent = co_await tx.send(42);

        // Receive
        auto value = co_await rx.recv();
        if (value) {
            received = true;
            received_value = *value;
        }

        loop.stop();
    };

    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        task().start();
    });

    loop.run();

    test("Value sent successfully", sent);
    test("Value received successfully", received);
    test("Received correct value", received_value == 42);
}

void test_channel_capacity() {
    std::cout << "\n--- Channel Capacity Test ---\n";

    EventLoop loop;
    int sends_completed = 0;

    auto task = [&]() -> Task<void> {
        auto [tx, rx] = channel<int>(3);

        // Fill the channel
        co_await tx.send(1);
        sends_completed++;
        co_await tx.send(2);
        sends_completed++;
        co_await tx.send(3);
        sends_completed++;

        // Channel should be full now
        test("Channel at capacity", tx.size() == tx.capacity());

        // Drain the channel
        int sum = 0;
        for (int i = 0; i < 3; i++) {
            auto value = co_await rx.recv();
            if (value) sum += *value;
        }

        test("Drained correct values", sum == 6);

        loop.stop();
    };

    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        task().start();
    });

    loop.run();

    test("All sends completed", sends_completed == 3);
}

void test_channel_blocking_send() {
    std::cout << "\n--- Blocking Send Test ---\n";

    EventLoop loop;
    std::atomic<int> sends_completed{0};
    std::atomic<int> receives_completed{0};

    auto sender = [&](Sender<int> tx) -> Task<void> {
        // Try to send 5 items to a channel with capacity 2
        for (int i = 0; i < 5; i++) {
            bool ok = co_await tx.send(i);
            if (ok) sends_completed++;
        }
        tx.close();
    };

    auto receiver = [&](Receiver<int> rx) -> Task<void> {
        // Slowly receive items
        while (auto value = co_await rx.recv()) {
            receives_completed++;
            co_await sleep(Duration::fromMillis(10), EventLoop::current());
        }
    };

    auto task = [&]() -> Task<void> {
        auto [tx, rx] = channel<int>(2);

        sender(tx).start();
        receiver(rx).start();

        // Let them run for a bit
        co_await sleep(Duration::fromMillis(100), EventLoop::current());

        loop.stop();
    };

    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        task().start();
    });

    loop.run();

    test("Sender blocked and resumed", sends_completed == 5);
    test("Receiver got all items", receives_completed == 5);
}

void test_channel_close() {
    std::cout << "\n--- Channel Close Test ---\n";

    EventLoop loop;
    bool send_after_close_failed = false;
    bool recv_after_close_none = false;

    auto task = [&]() -> Task<void> {
        auto [tx, rx] = channel<int>(5);

        // Send some values
        co_await tx.send(1);
        co_await tx.send(2);
        co_await tx.send(3);

        // Close the channel
        tx.close();

        // Try to send after close
        bool ok = co_await tx.send(99);
        send_after_close_failed = !ok;

        // Drain existing values
        int count = 0;
        while (auto value = co_await rx.recv()) {
            count++;
        }

        test("Drained 3 values before close", count == 3);

        // Try to receive after draining closed channel
        auto value = co_await rx.recv();
        recv_after_close_none = !value.has_value();

        loop.stop();
    };

    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        task().start();
    });

    loop.run();

    test("Send after close failed", send_after_close_failed);
    test("Recv after close returned None", recv_after_close_none);
}

void test_mpmc_multiple_senders() {
    std::cout << "\n--- Multiple Senders Test ---\n";

    EventLoop loop;
    std::atomic<int> total_received{0};

    auto sender = [](Sender<int> tx, int start, int count) -> Task<void> {
        for (int i = 0; i < count; i++) {
            co_await tx.send(start + i);
        }
    };

    auto receiver = [&](Receiver<int> rx) -> Task<void> {
        while (auto value = co_await rx.recv()) {
            total_received++;
        }
    };

    auto task = [&]() -> Task<void> {
        auto [tx, rx] = channel<int>(10);

        // Start 3 senders
        sender(tx, 0, 10).start();
        sender(tx, 100, 10).start();
        sender(tx, 200, 10).start();

        // Start receiver
        receiver(rx).start();

        // Wait for all sends to complete
        co_await sleep(Duration::fromMillis(50), EventLoop::current());

        tx.close();

        // Wait for receiver to drain
        co_await sleep(Duration::fromMillis(50), EventLoop::current());

        loop.stop();
    };

    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        task().start();
    });

    loop.run();

    test("Received all 30 items from 3 senders", total_received == 30);
}

void test_mpmc_multiple_receivers() {
    std::cout << "\n--- Multiple Receivers Test ---\n";

    EventLoop loop;
    std::atomic<int> receiver1_count{0};
    std::atomic<int> receiver2_count{0};
    std::atomic<int> receiver3_count{0};

    auto sender = [](Sender<int> tx) -> Task<void> {
        for (int i = 0; i < 30; i++) {
            co_await tx.send(i);
        }
        tx.close();
    };

    auto receiver = [](Receiver<int> rx, std::atomic<int>* counter) -> Task<void> {
        while (auto value = co_await rx.recv()) {
            (*counter)++;
        }
    };

    auto task = [&]() -> Task<void> {
        auto [tx, rx] = channel<int>(10);

        // Start sender
        sender(tx).start();

        // Start 3 receivers
        receiver(rx, &receiver1_count).start();
        receiver(rx, &receiver2_count).start();
        receiver(rx, &receiver3_count).start();

        // Wait for everything to complete
        co_await sleep(Duration::fromMillis(100), EventLoop::current());

        loop.stop();
    };

    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        task().start();
    });

    loop.run();

    int total = receiver1_count + receiver2_count + receiver3_count;
    test("All 30 items received across 3 receivers", total == 30);
    test("Work distributed among receivers",
         receiver1_count > 0 && receiver2_count > 0 && receiver3_count > 0);
}

void test_try_send_recv() {
    std::cout << "\n--- Try Send/Recv Test ---\n";

    auto [tx, rx] = channel<int>(2);

    // Try send to empty channel
    bool send1 = tx.trySend(1);
    bool send2 = tx.trySend(2);

    test("First trySend succeeded", send1);
    test("Second trySend succeeded", send2);

    // Channel is now full (capacity 2)
    bool send3 = tx.trySend(3);
    test("trySend to full channel failed", !send3);

    // Try receive
    auto value1 = rx.tryRecv();
    auto value2 = rx.tryRecv();

    test("First tryRecv succeeded", value1.has_value() && *value1 == 1);
    test("Second tryRecv succeeded", value2.has_value() && *value2 == 2);

    // Channel is now empty
    auto value3 = rx.tryRecv();
    test("tryRecv from empty channel returned None", !value3.has_value());
}

void test_producer_consumer_pattern() {
    std::cout << "\n--- Producer/Consumer Pattern Test ---\n";

    EventLoop loop;
    std::atomic<int> items_produced{0};
    std::atomic<int> items_consumed{0};
    std::vector<int> consumed_values;

    auto producer = [&](Sender<int> tx) -> Task<void> {
        for (int i = 0; i < 20; i++) {
            co_await tx.send(i);
            items_produced++;
            if (i % 5 == 0) {
                co_await sleep(Duration::fromMillis(5), EventLoop::current());
            }
        }
        tx.close();
    };

    auto consumer = [&](Receiver<int> rx) -> Task<void> {
        while (auto value = co_await rx.recv()) {
            consumed_values.push_back(*value);
            items_consumed++;
            co_await sleep(Duration::fromMillis(2), EventLoop::current());
        }
    };

    auto task = [&]() -> Task<void> {
        auto [tx, rx] = channel<int>(5);

        producer(tx).start();
        consumer(rx).start();

        // Wait for completion
        co_await sleep(Duration::fromMillis(200), EventLoop::current());

        loop.stop();
    };

    loop.addTimer(std::chrono::milliseconds(1), [&]() {
        task().start();
    });

    loop.run();

    test("Produced 20 items", items_produced == 20);
    test("Consumed 20 items", items_consumed == 20);

    // Verify order is maintained
    bool ordered = true;
    for (size_t i = 0; i < consumed_values.size(); i++) {
        if (consumed_values[i] != static_cast<int>(i)) {
            ordered = false;
            break;
        }
    }
    test("Values received in order", ordered);
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              Async Channel Test Suite                        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    try {
        test_basic_send_recv();
        test_channel_capacity();
        test_channel_blocking_send();
        test_channel_close();
        test_mpmc_multiple_senders();
        test_mpmc_multiple_receivers();
        test_try_send_recv();
        test_producer_consumer_pattern();
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
