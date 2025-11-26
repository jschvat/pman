#include <thread>

#include <sys/syscall.h>
#include <unistd.h>

#include "pman/observability.hpp"

int main() {
    auto snapshot = pman::snapshotProcess();
    if (snapshot.pid <= 0 || snapshot.name.empty()) {
        return 1;
    }
    if (snapshot.threads.empty()) {
        return 2;
    }
    bool foundSelf = false;
    for (const auto& thread : snapshot.threads) {
        if (thread.tid == syscall(SYS_gettid)) {
            foundSelf = true;
            break;
        }
    }
    if (!foundSelf) {
        return 3;
    }
    auto threadStats = pman::snapshotThread(0, 0);
    if (threadStats.tid <= 0 || threadStats.name.empty()) {
        return 4;
    }
    return 0;
}
