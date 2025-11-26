#include <chrono>
#include <iostream>
#include <vector>

#include "pman/rt_memory.hpp"

int main() {
    try {
        std::cout << "Locking current working set via mlockall...\n";
        pman::lockMemory();
    } catch (const std::exception& ex) {
        std::cout << "mlockall unavailable in this environment: " << ex.what() << "\n";
    }

    std::vector<char> buffer(1 << 20, 0);
    try {
        pman::adviseWillNeed(buffer.data(), buffer.size());
        pman::enableHugePages(buffer.data(), buffer.size());
        std::cout << "Advised kernel about buffer hotness + huge pages\n";
    } catch (const std::exception& ex) {
        std::cout << "madvise failed: " << ex.what() << "\n";
    }

    std::cout << "Prefaulting 64 KiB of stack...\n";
    pman::prefaultStack(64 * 1024);

    try {
        pman::setGuardSize(32 * 1024);
        std::cout << "Requested 32 KiB guard pages for future pthreads\n";
    } catch (const std::exception& ex) {
        std::cout << "Guard configuration failed: " << ex.what() << "\n";
    }

    return 0;
}
