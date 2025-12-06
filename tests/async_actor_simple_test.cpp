/// @file async_actor_simple_test.cpp
/// @brief Simple actor test to debug hangs

#include <iostream>
#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/actor.hpp"
#include "pman/async/sleep.hpp"

using namespace pman::async;

class SimpleActor : public Actor {
protected:
    Task<void> run(ActorContext& ctx) override {
        std::cout << "SimpleActor running with PID " << ctx.self_pid << "\n";
        auto msg = co_await receive();
        if (msg) {
            std::cout << "Received message!\n";
        }
        std::cout << "SimpleActor exiting\n";
    }
};

int main() {
    std::cout << "Starting simple actor test...\n";

    EventLoop loop;
    ProcessRegistry registry(&loop);

    std::cout << "Spawning actor...\n";
    auto pid = registry.spawn<SimpleActor>();
    std::cout << "Spawned PID: " << pid << "\n";

    auto sender_task = [&]() -> Task<void> {
        std::cout << "Sending message...\n";
        auto payload = std::make_shared<int>(42);
        bool sent = co_await registry.send(pid, INVALID_PID, payload);
        std::cout << "Send result: " << sent << "\n";

        co_await sleep(Duration::fromMillis(100), EventLoop::current());

        std::cout << "Stopping loop...\n";
        EventLoop::current()->stop();
    };

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        sender_task().start();
    });

    std::cout << "Running loop...\n";
    loop.run();

    std::cout << "Test complete!\n";
    return 0;
}
