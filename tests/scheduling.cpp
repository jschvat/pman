#include <chrono>

#include "pman/scheduling.hpp"

int main() {
    using namespace std::chrono;

    auto start = steady_clock::now();
    pman::sleep_for_precise(5ms);
    auto elapsed = steady_clock::now() - start;
    if (elapsed < 4ms) {
        return 1;
    }

    try {
        pman::set_timer_slack(100ns);
    } catch (...) {
        return 2;
    }

    return 0;
}
