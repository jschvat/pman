#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pman/cpu_set.hpp"
#include "pman/pipe.hpp"
#include "pman/process.hpp"

namespace pman {

class ProcessBuilder {
public:
    struct RedirectControl {
        bool closeParentAfterSpawn{false};
    };

    explicit ProcessBuilder(std::string executable);

    ProcessBuilder& arg(std::string value);
    ProcessBuilder& args(std::vector<std::string> values);
    ProcessBuilder& inheritEnvironment(bool inherit);
    ProcessBuilder& environment(std::vector<std::string> env);
    ProcessBuilder& workingDirectory(std::string path);
    ProcessBuilder& cpuAffinity(const CpuSet& set);
    ProcessBuilder& numaNode(int nodeId);
    ProcessBuilder& niceValue(int value);
    ProcessBuilder& searchPath(bool enabled);
    ProcessBuilder& newSession(bool enabled);
    ProcessBuilder& parentDeathSignal(int signal);
    ProcessBuilder& cgroup(std::string path);
    ProcessBuilder& ionice(int klass, int priority);
    ProcessBuilder& usePosixSpawn(bool enabled = true);

    ProcessBuilder& stdinFd(int fd, bool closeParentAfterSpawn = false, bool closeChildAfterDup = true);
    ProcessBuilder& stdoutFd(int fd, bool closeParentAfterSpawn = false, bool closeChildAfterDup = true);
    ProcessBuilder& stderrFd(int fd, bool closeParentAfterSpawn = false, bool closeChildAfterDup = true);

    ProcessBuilder& clearArguments();
    ProcessBuilder& clearEnvironment();

    Pipe captureStdout();
    Pipe captureStderr();
    Pipe captureStdin();

    ProcessHandle spawn();

private:

    ProcessConfig config_{};
    RedirectControl stdinCtrl_;
    RedirectControl stdoutCtrl_;
    RedirectControl stderrCtrl_;

    std::vector<int> parentCloseFds_;
};

}  // namespace pman
