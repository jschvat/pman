#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "pman/structured.hpp"

int main() {
    using namespace std::chrono_literals;

    // Basic cooperative cancellation
    {
        pman::TaskScope scope;
        std::atomic<int> iterations{0};
        scope.spawn([&](std::stop_token token) {
            while (!token.stop_requested()) {
                ++iterations;
                std::this_thread::sleep_for(1ms);
            }
        });
        std::this_thread::sleep_for(5ms);
        scope.cancel();
        scope.join();
        if (iterations.load() == 0) {
            return 1;
        }
    }

    // Exception propagation
    bool threw = false;
    {
        pman::TaskScope scope;
        scope.spawn([](std::stop_token) {
            throw std::runtime_error("boom");
        });
        scope.spawn([](std::stop_token token) {
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
        });
        try {
            scope.join();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) {
            return 2;
        }
    }

    // Deadline helper
    bool cancelled = false;
    pman::with_deadline(5ms, [&](std::stop_token token) {
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        cancelled = true;
        return 0;
    });
    if (!cancelled) {
        return 3;
    }

    return 0;
}
