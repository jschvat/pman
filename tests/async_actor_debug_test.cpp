/// @file async_actor_debug_test.cpp
/// @brief Debug the hanging test

#include <iostream>
#include <atomic>
#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/actor.hpp"
#include "pman/async/sleep.hpp"

using namespace pman::async;

class ReceiverActor : public Actor {
public:
    ReceiverActor(std::atomic<bool>* flag, std::atomic<int>* val)
        : flag_(flag), val_(val) {}

protected:
    Task<void> run(ActorContext& ctx) override {
        std::cout << "ReceiverActor: Waiting for message...\n";
        auto msg = co_await receive();
        if (msg) {
            std::cout << "ReceiverActor: Got message!\n";
            if (auto* data = msg->as<int>()) {
                *flag_ = true;
                *val_ = *data;
                std::cout << "ReceiverActor: Value is " << *data << "\n";
            }
        }
        std::cout << "ReceiverActor: Exiting\n";
    }

private:
    std::atomic<bool>* flag_;
    std::atomic<int>* val_;
};

int main() {
    std::cout << "Starting debug test...\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    std::atomic<bool> received{false};
    std::atomic<int> received_value{0};

    std::cout << "Spawning receiver actor...\n";
    auto receiver_pid = registry.spawn<ReceiverActor>(&received, &received_value);
    std::cout << "Receiver PID: " << receiver_pid << "\n";

    auto sender_task = [&]() -> Task<void> {
        std::cout << "Sender: Starting...\n";
        auto payload = std::make_shared<int>(42);
        std::cout << "Sender: Sending message...\n";
        bool sent = co_await registry.send(receiver_pid, INVALID_PID, payload);
        std::cout << "Sender: Send result: " << sent << "\n";

        std::cout << "Sender: Sleeping...\n";
        co_await sleep(Duration::fromMillis(50), EventLoop::current());

        std::cout << "Sender: Stopping loop...\n";
        EventLoop::current()->stop();
    };

    std::cout << "Adding timer...\n";
    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        std::cout << "Timer: Starting sender task...\n";
        sender_task().start();
    });

    std::cout << "Running loop...\n";
    loop.run();

    std::cout << "Loop stopped\n";
    std::cout << "Received: " << received.load() << "\n";
    std::cout << "Value: " << received_value.load() << "\n";

    return 0;
}
