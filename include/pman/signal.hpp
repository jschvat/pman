#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <signal.h>
#include <sys/signalfd.h>
#include <sys/types.h>

namespace pman {

using SignalHandler = std::function<void(const signalfd_siginfo&)>;

class SignalDispatcher {
public:
    static SignalDispatcher& instance();

    void start(const std::vector<int>& signals);
    void stop();

    void registerHandler(int signal, SignalHandler handler);
    void unregisterHandler(int signal);

    void blockSignalsForCurrentThread();

private:
    SignalDispatcher() = default;
    ~SignalDispatcher();

    SignalDispatcher(const SignalDispatcher&) = delete;
    SignalDispatcher& operator=(const SignalDispatcher&) = delete;

    void run();

    int fd_{-1};
    bool running_{false};
    sigset_t mask_{};
    std::thread thread_;
    std::mutex mutex_;
    std::unordered_map<int, SignalHandler> handlers_;
};

void blockSignalsInCurrentThread(const std::vector<int>& signals);

bool sendSignalSequence(pid_t target,
                        const std::vector<std::pair<int, std::chrono::nanoseconds>>& sequence,
                        bool processGroup = false);

struct GracefulShutdownOptions {
    std::chrono::nanoseconds termTimeout{std::chrono::milliseconds(250)};
    std::chrono::nanoseconds intTimeout{std::chrono::milliseconds(250)};
    std::chrono::nanoseconds hupTimeout{std::chrono::milliseconds(250)};
    bool sendInt{true};
    bool sendHup{true};
    bool processGroup{false};
};

bool gracefulShutdown(pid_t pid, const GracefulShutdownOptions& options = {});

}  // namespace pman
