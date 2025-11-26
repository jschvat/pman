#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <pthread.h>
#include <sys/types.h>

namespace pman {

enum class ThreadEventType { Created, Started, Finished, Exception };

struct ThreadEvent {
    ThreadEventType type{};
    std::string name;
    pthread_t handle{};
    std::chrono::steady_clock::time_point timestamp;
    std::string message;
};

using ThreadObserver = std::function<void(const ThreadEvent&)>;

void setThreadObserver(ThreadObserver observer);

enum class ProcessEventType { LaunchAttempt, LaunchSuccess, LaunchFailure, Exit };

struct ProcessEvent {
    ProcessEventType type{};
    pid_t pid{-1};
    std::string executable;
    int status{0};
    int error{0};
    std::chrono::steady_clock::time_point timestamp;
    std::string message;
};

using ProcessObserver = std::function<void(const ProcessEvent&)>;

void setProcessObserver(ProcessObserver observer);

namespace detail {

void emitThreadEvent(const ThreadEvent& event);
void emitProcessEvent(const ProcessEvent& event);

}  // namespace detail

}  // namespace pman
