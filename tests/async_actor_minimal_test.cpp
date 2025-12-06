///  @file async_actor_minimal_test.cpp
/// @brief Minimal actor test to find segfault

#include <iostream>
#include "pman/async/event_loop.hpp"
#include "pman/async/actor.hpp"

using namespace pman::async;

class MinimalActor : public Actor {
protected:
    Task<void> run(ActorContext& ctx) override {
        std::cout << "Actor running\n";
        co_return;
    }
};

int main() {
    std::cout << "Creating event loop...\n";
    EventLoop loop;

    std::cout << "Creating registry...\n";
    ProcessRegistry registry(&loop);

    std::cout << "Spawning actor...\n";
    auto pid = registry.spawn<MinimalActor>();
    std::cout << "Spawned PID: " << pid << "\n";

    std::cout << "Adding timer...\n";
    loop.addTimer(std::chrono::milliseconds(100), [&]() {
        std::cout << "Timer fired, stopping loop\n";
        loop.stop();
    });

    std::cout << "Running loop...\n";
    loop.run();

    std::cout << "Test complete!\n";
    return 0;
}
