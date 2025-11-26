#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

#include "pman/cpu_set.hpp"

namespace pman {

struct ProcessConfig {
    std::string executable;
    std::vector<std::string> arguments;
    std::vector<std::string> environment;
    bool inheritEnvironment{true};
    std::optional<std::string> workingDirectory;
    std::optional<CpuSet> cpuAffinity;
    std::optional<int> numaNode;
    std::optional<int> niceValue;
    bool searchPath{true};
    struct FdRedirect {
        std::optional<int> fd;
        bool closeAfterDup{false};
    };
    FdRedirect stdinRedirect;
    FdRedirect stdoutRedirect;
    FdRedirect stderrRedirect;
    bool newSession{false};
    std::optional<int> parentDeathSignal;
    std::vector<int> parentCloseFds;
    std::optional<std::string> cgroupPath;
    std::optional<int> ioniceClass;
    std::optional<int> ionicePriority;
    bool usePosixSpawn{false};
};

class ProcessHandle {
public:
    ProcessHandle() = default;
    explicit ProcessHandle(pid_t pid, std::string executable = {}, int pidfd = -1)
        : pid_(pid), executable_(std::move(executable)), pidfd_(pidfd) {}
    ~ProcessHandle();

    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;

    ProcessHandle(ProcessHandle&& other) noexcept;
    ProcessHandle& operator=(ProcessHandle&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return pid_ > 0; }
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    [[nodiscard]] const std::string& executable() const noexcept { return executable_; }
    [[nodiscard]] int pidfd() const noexcept { return pidfd_; }

    void sendSignal(int signal) const;
    void terminate() const;
    void kill() const;

    int wait();
    bool tryWait(int& status);
    bool waitFor(std::chrono::nanoseconds timeout);
    bool waitFor(std::chrono::nanoseconds timeout, int& status);

    bool terminate(std::chrono::nanoseconds gracePeriod);

    void setCpuAffinity(const CpuSet& set) const;
    void setNiceValue(int value) const;
    void attachToCgroup(const std::string& path) const;
    void setIoPriority(int klass, int priority) const;

private:
    void closePidfd();

    pid_t pid_{-1};
    std::string executable_;
    int pidfd_{-1};
};

ProcessHandle launchProcess(const ProcessConfig& config);

}  // namespace pman
