#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pman/process.hpp"
#include "pman/thread.hpp"

namespace pman {

enum class RestartPolicy {
    Never,
    Always,
    OnFailure,
    OnSuccess,
};

enum class BackoffStrategy {
    Exponential,
    Linear,
    Custom,
};

struct RestartConfig {
    RestartPolicy policy{RestartPolicy::OnFailure};
    BackoffStrategy backoffStrategy{BackoffStrategy::Exponential};
    std::chrono::nanoseconds minRestartDelay{std::chrono::seconds(1)};
    std::chrono::nanoseconds maxRestartDelay{std::chrono::minutes(5)};
    double backoffMultiplier{2.0};
    std::chrono::nanoseconds linearIncrement{std::chrono::seconds(1)};
    std::function<std::chrono::nanoseconds(int /*restartCount*/)> customBackoff;
    int maxRestarts{-1};
    std::chrono::nanoseconds maxRestartWindow{std::chrono::hours(1)};
    int maxRestartsInWindow{10};
};

struct SupervisedProcess {
    std::string name;
    ProcessConfig config;
    RestartConfig restart;
    std::function<void(int /*status*/)> onExit;
    std::function<void()> onRestart;
};

struct ProcessStatus {
    std::string name;
    pid_t pid{-1};
    bool running{false};
    int lastExitStatus{0};
    int restartCount{0};
    std::chrono::steady_clock::time_point lastStartTime;
    std::chrono::steady_clock::time_point lastExitTime;
    std::chrono::nanoseconds nextRestartDelay{0};
};

class ProcessSupervisor {
public:
    ProcessSupervisor();
    ~ProcessSupervisor();

    ProcessSupervisor(const ProcessSupervisor&) = delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

    void add(SupervisedProcess spec);
    void remove(const std::string& name);

    void start();
    void stop();
    void stopGracefully(std::chrono::nanoseconds timeout);

    void startProcess(const std::string& name);
    void stopProcess(const std::string& name);
    void restartProcess(const std::string& name);

    [[nodiscard]] std::vector<ProcessStatus> status() const;
    [[nodiscard]] ProcessStatus status(const std::string& name) const;

    void onProcessExit(std::function<void(const std::string&, int)> callback);
    void onProcessStart(std::function<void(const std::string&, pid_t)> callback);

private:
    struct ProcessState {
        SupervisedProcess spec;
        std::optional<ProcessHandle> handle;
        ProcessStatus status;
        std::vector<std::chrono::steady_clock::time_point> restartTimes;
        bool pendingRestart{false};
        std::chrono::steady_clock::time_point scheduledRestartTime;
    };

    void supervisorLoop();
    void handleChildExit(pid_t pid, int status);
    bool shouldRestart(const std::string& name, int status);
    std::chrono::nanoseconds calculateRestartDelay(const std::string& name);
    void doStartProcess(ProcessState& state);
    void cleanupRestartHistory(ProcessState& state);

    std::unordered_map<std::string, ProcessState> processes_;
    std::unordered_map<pid_t, std::string> pidToName_;

    mutable std::mutex mutex_;
    std::unique_ptr<ManagedThread> supervisorThread_;
    int signalFd_{-1};
    std::atomic<bool> running_{false};

    std::function<void(const std::string&, int)> exitCallback_;
    std::function<void(const std::string&, pid_t)> startCallback_;
};

}  // namespace pman
