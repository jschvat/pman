#include <iostream>
#include <vector>

#include "pman/rt_memory.hpp"

int main() {
    std::vector<char> buffer(4096);
    try {
        pman::adviseWillNeed(buffer.data(), buffer.size());
    } catch (const std::exception&) {
        // ignore in constrained environments
    }
    try {
        pman::enableHugePages(buffer.data(), buffer.size());
    } catch (const std::exception&) {
        // ignore lack of permissions
    }
    pman::prefaultStack(8192);
    return 0;
}
