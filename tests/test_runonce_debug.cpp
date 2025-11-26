#include "pman/async/event_loop.hpp"
#include <iostream>
#include <chrono>

int main() {
    using namespace pman::async;

    EventLoop loop;
    int count = 0;

    loop.addTimer(std::chrono::milliseconds(10), [&]() {
        std::cout << "Timer 1 fired!" << std::endl;
        count++;
    });
    loop.addTimer(std::chrono::milliseconds(20), [&]() {
        std::cout << "Timer 2 fired!" << std::endl;
        count++;
    });

    std::cout << "Running 10 iterations of runOnce(5ms)..." << std::endl;
    for (int i = 0; i < 10; i++) {
        std::cout << "Iteration " << i << ", count=" << count << std::endl;
        loop.runOnce(std::chrono::milliseconds(5));
    }

    std::cout << "Final count: " << count << " (expected 2)" << std::endl;
    return count == 2 ? 0 : 1;
}
