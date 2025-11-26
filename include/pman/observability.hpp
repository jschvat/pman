#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <sys/types.h>

namespace pman {

struct ThreadStats {
    pid_t tid{-1};
    std::string name;
    char state{'?'};
    long long userTimeTicks{0};
    long long systemTimeTicks{0};
    long long voluntaryContextSwitches{0};
    long long involuntaryContextSwitches{0};
};

struct ProcessSnapshot {
    pid_t pid{-1};
    std::string name;
    long long startTimeTicks{0};
    long long userTimeTicks{0};
    long long systemTimeTicks{0};
    std::vector<ThreadStats> threads;
};

ProcessSnapshot snapshotProcess(pid_t pid = 0);
ThreadStats snapshotThread(pid_t pid, pid_t tid);

}  // namespace pman
