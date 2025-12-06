/// @file async_actor_test.cpp
/// @brief Comprehensive test suite for BEAM/Erlang-style actor system

#include <iostream>
#include <vector>
#include <atomic>
#include <chrono>
#include <unordered_set>

#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/actor.hpp"
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

// ============================================================================
// Test Actors
// ============================================================================

/// Simple echo actor - sends back what it receives
class EchoActor : public Actor {
protected:
    Task<void> run(ActorContext& ctx) override {
        while (true) {
            auto msg = co_await receive();
            if (!msg) break;

            // Echo back to sender
            if (msg->from != INVALID_PID) {
                co_await ctx.send(msg->from, msg->payload);
            }
        }
    }
};

/// Counter actor - maintains a counter and responds to increment/get requests
struct IncrementMsg {};
struct GetCountMsg {};
struct CountReply { int count; };

class CounterActor : public Actor {
public:
    CounterActor() : count_(0) {}

protected:
    Task<void> run(ActorContext& ctx) override {
        while (true) {
            auto msg = co_await receive();
            if (!msg) break;

            if (msg->as<IncrementMsg>()) {
                count_++;
            } else if (msg->as<GetCountMsg>()) {
                auto reply = std::make_shared<CountReply>();
                reply->count = count_;
                co_await ctx.send(msg->from, reply);
            }
        }
    }

private:
    int count_;
};

/// Crasher actor - crashes after receiving N messages
class CrasherActor : public Actor {
public:
    CrasherActor(int crash_after = 1) : crash_after_(crash_after) {}

protected:
    Task<void> run(ActorContext& ctx) override {
        int count = 0;
        while (true) {
            auto msg = co_await receive();
            if (!msg) break;

            count++;
            if (count >= crash_after_) {
                throw std::runtime_error("Intentional crash");
            }
        }
    }

private:
    int crash_after_;
};

/// Monitor watcher - records DOWN messages
class MonitorWatcher : public Actor {
public:
    std::atomic<int> down_count{0};
    std::atomic<ProcessId> last_down_pid{INVALID_PID};

protected:
    Task<void> run(ActorContext& ctx) override {
        while (true) {
            auto msg = co_await receive();
            if (!msg) break;

            if (auto* down = msg->as<DownMessage>()) {
                down_count++;
                last_down_pid.store(down->pid);
            }
        }
    }
};

// ============================================================================
// Basic Tests
// ============================================================================

void test_basic_spawn() {
    std::cout << "\n--- Basic Spawn Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto pid = registry.spawn<EchoActor>();

    test("Spawned actor has valid PID", pid != INVALID_PID);
    test("Can get actor from registry", registry.getActor(pid) != nullptr);

    // No need to run the loop for this test
}

void test_basic_send_receive() {
    std::cout << "\n--- Basic Send/Receive Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    std::atomic<bool> received{false};
    std::atomic<int> received_value{0};

    class ReceiverActor : public Actor {
    public:
        ReceiverActor(std::atomic<bool>* flag, std::atomic<int>* val)
            : flag_(flag), val_(val) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            auto msg = co_await receive();
            if (msg) {
                if (auto* data = msg->as<int>()) {
                    *flag_ = true;
                    *val_ = *data;
                }
            }
        }

    private:
        std::atomic<bool>* flag_;
        std::atomic<int>* val_;
    };

    auto receiver_pid = registry.spawn<ReceiverActor>(&received, &received_value);

    auto sender_task = [&]() -> Task<void> {
        auto payload = std::make_shared<int>(42);
        bool sent = co_await registry.send(receiver_pid, INVALID_PID, payload);
        test("Send succeeded", sent);

        co_await sleep(Duration::fromMillis(50), EventLoop::current());

        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        sender_task().start();
    });

    loop.run();

    test("Message was received", received.load());
    test("Correct value received", received_value.load() == 42);
}

void test_echo_actor() {
    std::cout << "\n--- Echo Actor Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto echo_pid = registry.spawn<EchoActor>();

    std::atomic<bool> got_reply{false};
    std::atomic<int> reply_value{0};

    class ClientActor : public Actor {
    public:
        ClientActor(ProcessId echo, std::atomic<bool>* flag, std::atomic<int>* val)
            : echo_pid_(echo), flag_(flag), val_(val) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            // Send to echo
            auto payload = std::make_shared<int>(99);
            co_await ctx.send(echo_pid_, payload);

            // Wait for reply
            auto msg = co_await receive();
            if (msg) {
                if (auto* data = msg->as<int>()) {
                    *flag_ = true;
                    *val_ = *data;
                }
            }
        }

    private:
        ProcessId echo_pid_;
        std::atomic<bool>* flag_;
        std::atomic<int>* val_;
    };

    auto client_pid = registry.spawn<ClientActor>(echo_pid, &got_reply, &reply_value);

    loop.addTimer(std::chrono::milliseconds(100), [&]() {
        loop.stop();
    });

    loop.run();

    test("Got echo reply", got_reply.load());
    test("Echo value correct", reply_value.load() == 99);
}

void test_counter_actor() {
    std::cout << "\n--- Counter Actor Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto counter_pid = registry.spawn<CounterActor>();

    std::atomic<int> final_count{-1};

    class ClientActor : public Actor {
    public:
        ClientActor(ProcessId counter, std::atomic<int>* result)
            : counter_pid_(counter), result_(result) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            // Increment 5 times
            for (int i = 0; i < 5; i++) {
                co_await ctx.send(counter_pid_, std::make_shared<IncrementMsg>());
            }

            // Get count
            co_await ctx.send(counter_pid_, std::make_shared<GetCountMsg>());

            auto msg = co_await receive();
            if (msg) {
                if (auto* reply = msg->as<CountReply>()) {
                    *result_ = reply->count;
                }
            }
        }

    private:
        ProcessId counter_pid_;
        std::atomic<int>* result_;
    };

    auto client_pid = registry.spawn<ClientActor>(counter_pid, &final_count);

    loop.addTimer(std::chrono::milliseconds(100), [&]() {
        loop.stop();
    });

    loop.run();

    test("Counter incremented correctly", final_count.load() == 5);
}

// ============================================================================
// Registry Tests
// ============================================================================

void test_register_name() {
    std::cout << "\n--- Register Name Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto pid = registry.spawn<EchoActor>();

    bool registered = registry.registerName("echo_server", pid);
    test("Name registration succeeded", registered);

    auto found_pid = registry.whereis("echo_server");
    test("Whereis found process", found_pid.has_value());
    test("Whereis returned correct PID", found_pid.value() == pid);

    // Try to register same name again
    auto pid2 = registry.spawn<EchoActor>();
    bool registered2 = registry.registerName("echo_server", pid2);
    test("Duplicate name registration failed", !registered2);

    loop.stop();
}

void test_unregister_name() {
    std::cout << "\n--- Unregister Name Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto pid = registry.spawn<EchoActor>();
    registry.registerName("temp_server", pid);

    auto found1 = registry.whereis("temp_server");
    test("Name registered", found1.has_value());

    registry.unregisterName("temp_server");

    auto found2 = registry.whereis("temp_server");
    test("Name unregistered", !found2.has_value());

    loop.stop();
}

void test_send_to_named_process() {
    std::cout << "\n--- Send to Named Process Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    std::atomic<bool> received{false};

    class NamedActor : public Actor {
    public:
        NamedActor(std::atomic<bool>* flag) : flag_(flag) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            auto msg = co_await receive();
            if (msg) {
                *flag_ = true;
            }
        }

    private:
        std::atomic<bool>* flag_;
    };

    auto pid = registry.spawn<NamedActor>(&received);
    registry.registerName("named_actor", pid);

    auto sender_task = [&]() -> Task<void> {
        auto payload = std::make_shared<int>(1);
        bool sent = co_await registry.send("named_actor", INVALID_PID, payload);
        test("Send to named process succeeded", sent);

        co_await sleep(Duration::fromMillis(50), EventLoop::current());
        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        sender_task().start();
    });

    loop.run();

    test("Named process received message", received.load());
}

// ============================================================================
// Link and Monitor Tests
// ============================================================================

void test_process_links() {
    std::cout << "\n--- Process Links Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    std::atomic<bool> linked_crashed{false};

    class LinkedActor : public Actor {
    public:
        LinkedActor(std::atomic<bool>* flag) : flag_(flag) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            while (true) {
                auto msg = co_await receive();
                if (!msg) break;
            }
        }

        Task<void> handleExitSignal(const ExitSignal& signal) override {
            *flag_ = true;
            co_return co_await Actor::handleExitSignal(signal);
        }

    private:
        std::atomic<bool>* flag_;
    };

    auto crasher = registry.spawn<CrasherActor>(1);
    auto linked = registry.spawn<LinkedActor>(&linked_crashed);

    // Link them
    registry.link(crasher, linked);

    auto trigger_task = [&]() -> Task<void> {
        // Trigger crasher to crash
        auto payload = std::make_shared<int>(1);
        co_await registry.send(crasher, INVALID_PID, payload);

        // Wait for crash propagation
        co_await sleep(Duration::fromMillis(100), EventLoop::current());

        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        trigger_task().start();
    });

    loop.run();

    test("Linked process received exit signal", linked_crashed.load());
}

void test_process_monitors() {
    std::cout << "\n--- Process Monitors Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto crasher = registry.spawn<CrasherActor>(1);
    auto watcher = registry.spawn<MonitorWatcher>();

    auto watcher_actor = std::static_pointer_cast<MonitorWatcher>(registry.getActor(watcher));

    auto test_task = [&]() -> Task<void> {
        // Set up monitor
        auto ref = co_await registry.getActor(watcher)->getContext().monitor(crasher);
        test("Monitor reference created", ref != INVALID_PID);

        // Trigger crasher to crash
        auto payload = std::make_shared<int>(1);
        co_await registry.send(crasher, INVALID_PID, payload);

        // Wait for DOWN message
        co_await sleep(Duration::fromMillis(100), EventLoop::current());

        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        test_task().start();
    });

    loop.run();

    test("Watcher received DOWN message", watcher_actor->down_count.load() > 0);
    test("DOWN message has correct PID", watcher_actor->last_down_pid.load() == crasher);
}

void test_multiple_links() {
    std::cout << "\n--- Multiple Links Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    std::atomic<int> crash_count{0};

    class MultiLinkedActor : public Actor {
    public:
        MultiLinkedActor(std::atomic<int>* counter) : counter_(counter) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            while (true) {
                auto msg = co_await receive();
                if (!msg) break;
            }
        }

        Task<void> handleExitSignal(const ExitSignal& signal) override {
            (*counter_)++;
            co_return co_await Actor::handleExitSignal(signal);
        }

    private:
        std::atomic<int>* counter_;
    };

    auto crasher = registry.spawn<CrasherActor>(1);
    auto linked1 = registry.spawn<MultiLinkedActor>(&crash_count);
    auto linked2 = registry.spawn<MultiLinkedActor>(&crash_count);
    auto linked3 = registry.spawn<MultiLinkedActor>(&crash_count);

    // Link all to crasher
    registry.link(crasher, linked1);
    registry.link(crasher, linked2);
    registry.link(crasher, linked3);

    auto trigger_task = [&]() -> Task<void> {
        auto payload = std::make_shared<int>(1);
        co_await registry.send(crasher, INVALID_PID, payload);

        co_await sleep(Duration::fromMillis(150), EventLoop::current());

        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        trigger_task().start();
    });

    loop.run();

    test("All linked processes notified", crash_count.load() == 3);
}

// ============================================================================
// Stress Tests
// ============================================================================

void test_many_actors() {
    std::cout << "\n--- Many Actors Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    const int NUM_ACTORS = 100;
    std::vector<ProcessId> pids;

    for (int i = 0; i < NUM_ACTORS; i++) {
        pids.push_back(registry.spawn<EchoActor>());
    }

    test("Spawned 100 actors", pids.size() == NUM_ACTORS);

    // Verify all are unique
    std::unordered_set<ProcessId> unique_pids(pids.begin(), pids.end());
    test("All PIDs unique", unique_pids.size() == NUM_ACTORS);

    loop.stop();
}

void test_many_messages() {
    std::cout << "\n--- Many Messages Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    const int NUM_MESSAGES = 1000;
    std::atomic<int> received_count{0};

    class BulkReceiver : public Actor {
    public:
        BulkReceiver(std::atomic<int>* counter) : counter_(counter) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            for (int i = 0; i < 1000; i++) {
                auto msg = co_await receive();
                if (!msg) break;
                (*counter_)++;
            }
        }

    private:
        std::atomic<int>* counter_;
    };

    auto receiver = registry.spawn<BulkReceiver>(&received_count);

    auto sender_task = [&]() -> Task<void> {
        for (int i = 0; i < NUM_MESSAGES; i++) {
            auto payload = std::make_shared<int>(i);
            co_await registry.send(receiver, INVALID_PID, payload);
        }

        co_await sleep(Duration::fromMillis(200), EventLoop::current());

        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        sender_task().start();
    });

    loop.run();

    test("Received all 1000 messages", received_count.load() == NUM_MESSAGES);
}

void test_ping_pong() {
    std::cout << "\n--- Ping Pong Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    const int NUM_BOUNCES = 50;
    std::atomic<int> ping_count{0};
    std::atomic<int> pong_count{0};

    class PingActor : public Actor {
    public:
        PingActor(ProcessId pong, int bounces, std::atomic<int>* counter)
            : pong_pid_(pong), bounces_(bounces), counter_(counter) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            // Start the ping-pong
            co_await ctx.send(pong_pid_, std::make_shared<int>(0));

            for (int i = 0; i < bounces_; i++) {
                auto msg = co_await receive();
                if (!msg) break;

                (*counter_)++;

                if (auto* count = msg->as<int>()) {
                    if (*count < bounces_) {
                        co_await ctx.send(pong_pid_, std::make_shared<int>(*count + 1));
                    }
                }
            }
        }

    private:
        ProcessId pong_pid_;
        int bounces_;
        std::atomic<int>* counter_;
    };

    class PongActor : public Actor {
    public:
        PongActor(std::atomic<int>* counter) : counter_(counter) {}

        void setPing(ProcessId ping) { ping_pid_ = ping; }

    protected:
        Task<void> run(ActorContext& ctx) override {
            while (true) {
                auto msg = co_await receive();
                if (!msg) break;

                (*counter_)++;

                if (auto* count = msg->as<int>()) {
                    if (*count < 50 && ping_pid_ != INVALID_PID) {
                        co_await ctx.send(ping_pid_, std::make_shared<int>(*count + 1));
                    }
                }
            }
        }

    private:
        ProcessId ping_pid_{INVALID_PID};
        std::atomic<int>* counter_;
    };

    auto pong = registry.spawn<PongActor>(&pong_count);
    auto pong_actor = std::static_pointer_cast<PongActor>(registry.getActor(pong));

    auto ping = registry.spawn<PingActor>(pong, NUM_BOUNCES, &ping_count);
    pong_actor->setPing(ping);

    loop.addTimer(std::chrono::milliseconds(500), [&]() {
        loop.stop();
    });

    loop.run();

    test("Ping received messages", ping_count.load() > 0);
    test("Pong received messages", pong_count.load() > 0);
    test("Total bounces correct", ping_count.load() + pong_count.load() >= NUM_BOUNCES);
}

// ============================================================================
// Edge Cases
// ============================================================================

void test_send_to_dead_actor() {
    std::cout << "\n--- Send to Dead Actor Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto actor = registry.spawn<EchoActor>();
    auto actor_ptr = registry.getActor(actor);
    actor_ptr->stop();

    auto test_task = [&]() -> Task<void> {
        co_await sleep(Duration::fromMillis(50), EventLoop::current());

        auto payload = std::make_shared<int>(1);
        bool sent = co_await registry.send(actor, INVALID_PID, payload);

        test("Send to stopped actor failed", !sent);

        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        test_task().start();
    });

    loop.run();
}

void test_actor_self_exit() {
    std::cout << "\n--- Actor Self Exit Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    std::atomic<bool> exited{false};

    class SelfExitActor : public Actor {
    public:
        SelfExitActor(std::atomic<bool>* flag) : flag_(flag) {}

    protected:
        Task<void> run(ActorContext& ctx) override {
            *flag_ = true;
            // Exit normally
            co_return;
        }

    private:
        std::atomic<bool>* flag_;
    };

    auto actor = registry.spawn<SelfExitActor>(&exited);

    loop.addTimer(std::chrono::milliseconds(100), [&]() {
        loop.stop();
    });

    loop.run();

    test("Actor ran and exited", exited.load());
}

void test_whereis_nonexistent() {
    std::cout << "\n--- Whereis Nonexistent Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto pid = registry.whereis("does_not_exist");

    test("Whereis returned nullopt for nonexistent name", !pid.has_value());

    loop.stop();
}

void test_link_to_dead_actor() {
    std::cout << "\n--- Link to Dead Actor Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto actor1 = registry.spawn<EchoActor>();
    auto actor2 = registry.spawn<EchoActor>();

    // Stop actor2
    registry.getActor(actor2)->stop();

    // Try to link to dead actor (should not crash)
    registry.link(actor1, actor2);

    test("Link to dead actor did not crash", true);

    loop.stop();
}

void test_monitor_dead_actor() {
    std::cout << "\n--- Monitor Dead Actor Test ---\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    auto watcher = registry.spawn<MonitorWatcher>();
    auto dead = registry.spawn<EchoActor>();

    // Stop the actor
    registry.getActor(dead)->stop();

    auto test_task = [&]() -> Task<void> {
        co_await sleep(Duration::fromMillis(50), EventLoop::current());

        // Try to monitor dead actor
        auto ref = co_await registry.getActor(watcher)->getContext().monitor(dead);

        test("Monitor dead actor returned invalid reference", ref == INVALID_PID);

        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        test_task().start();
    });

    loop.run();
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║         BEAM/Erlang-Style Actor System Test Suite           ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    try {
        // Basic tests
        test_basic_spawn();
        test_basic_send_receive();
        test_echo_actor();
        test_counter_actor();

        // Registry tests
        test_register_name();
        test_unregister_name();
        test_send_to_named_process();

        // Link and monitor tests
        test_process_links();
        test_process_monitors();
        test_multiple_links();

        // Stress tests
        test_many_actors();
        test_many_messages();
        test_ping_pong();

        // Edge cases
        test_send_to_dead_actor();
        test_actor_self_exit();
        test_whereis_nonexistent();
        test_link_to_dead_actor();
        test_monitor_dead_actor();

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
