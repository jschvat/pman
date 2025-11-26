#include <chrono>
#include <iostream>
#include <thread>

#include "pman/structured.hpp"

int main() {
    using namespace std::chrono_literals;

    pman::TaskScope scope;
    scope.spawn([](std::stop_token token) {
        while (!token.stop_requested()) {
            std::cout << "background task heartbeat\n";
            std::this_thread::sleep_for(50ms);
        }
        std::cout << "background task canceled\n";
    });

    std::this_thread::sleep_for(120ms);
    scope.cancel();
    scope.join();

    std::cout << "Running work under with_deadline(...).\n";
    pman::with_deadline(100ms, [](std::stop_token token) {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(20ms);
        }
        std::cout << "deadline elapsed -> token requested\n";
        return 0;
    });
    return 0;
}
