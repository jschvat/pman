#include "pman/process_builder.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>

namespace pman {
namespace {

Pipe makeCapturedPipe(ProcessConfig::FdRedirect& redirect,
                      ProcessBuilder::RedirectControl& ctrl,
                      std::vector<int>& parentCloseFds,
                      bool toChildStdin) {
    Pipe pipe = Pipe::create();
    int childFd = toChildStdin ? pipe.releaseRead() : pipe.releaseWrite();
    if (childFd < 0) {
        throw std::runtime_error("pipe creation failed");
    }
    redirect.fd = childFd;
    redirect.closeAfterDup = true;
    ctrl.closeParentAfterSpawn = true;
    parentCloseFds.push_back(childFd);
    return pipe;
}

}  // namespace

ProcessBuilder::ProcessBuilder(std::string executable) {
    config_.executable = std::move(executable);
}

ProcessBuilder& ProcessBuilder::arg(std::string value) {
    config_.arguments.push_back(std::move(value));
    return *this;
}

ProcessBuilder& ProcessBuilder::args(std::vector<std::string> values) {
    for (auto& v : values) {
        config_.arguments.push_back(std::move(v));
    }
    return *this;
}

ProcessBuilder& ProcessBuilder::inheritEnvironment(bool inherit) {
    config_.inheritEnvironment = inherit;
    return *this;
}

ProcessBuilder& ProcessBuilder::environment(std::vector<std::string> env) {
    config_.environment = std::move(env);
    return *this;
}

ProcessBuilder& ProcessBuilder::workingDirectory(std::string path) {
    config_.workingDirectory = std::move(path);
    return *this;
}

ProcessBuilder& ProcessBuilder::cpuAffinity(const CpuSet& set) {
    config_.cpuAffinity = set;
    return *this;
}

ProcessBuilder& ProcessBuilder::numaNode(int nodeId) {
    config_.numaNode = nodeId;
    return *this;
}

ProcessBuilder& ProcessBuilder::niceValue(int value) {
    config_.niceValue = value;
    return *this;
}

ProcessBuilder& ProcessBuilder::searchPath(bool enabled) {
    config_.searchPath = enabled;
    return *this;
}

ProcessBuilder& ProcessBuilder::newSession(bool enabled) {
    config_.newSession = enabled;
    return *this;
}

ProcessBuilder& ProcessBuilder::parentDeathSignal(int signal) {
    config_.parentDeathSignal = signal;
    return *this;
}

ProcessBuilder& ProcessBuilder::cgroup(std::string path) {
    config_.cgroupPath = std::move(path);
    return *this;
}

ProcessBuilder& ProcessBuilder::ionice(int klass, int priority) {
    config_.ioniceClass = klass;
    config_.ionicePriority = priority;
    return *this;
}

ProcessBuilder& ProcessBuilder::usePosixSpawn(bool enabled) {
    config_.usePosixSpawn = enabled;
    return *this;
}

ProcessBuilder& ProcessBuilder::stdinFd(int fd, bool closeParentAfterSpawn, bool closeChildAfterDup) {
    config_.stdinRedirect.fd = fd;
    config_.stdinRedirect.closeAfterDup = closeChildAfterDup;
    stdinCtrl_.closeParentAfterSpawn = closeParentAfterSpawn;
    if (closeParentAfterSpawn) {
        parentCloseFds_.push_back(fd);
    }
    return *this;
}

ProcessBuilder& ProcessBuilder::stdoutFd(int fd, bool closeParentAfterSpawn, bool closeChildAfterDup) {
    config_.stdoutRedirect.fd = fd;
    config_.stdoutRedirect.closeAfterDup = closeChildAfterDup;
    stdoutCtrl_.closeParentAfterSpawn = closeParentAfterSpawn;
    if (closeParentAfterSpawn) {
        parentCloseFds_.push_back(fd);
    }
    return *this;
}

ProcessBuilder& ProcessBuilder::stderrFd(int fd, bool closeParentAfterSpawn, bool closeChildAfterDup) {
    config_.stderrRedirect.fd = fd;
    config_.stderrRedirect.closeAfterDup = closeChildAfterDup;
    stderrCtrl_.closeParentAfterSpawn = closeParentAfterSpawn;
    if (closeParentAfterSpawn) {
        parentCloseFds_.push_back(fd);
    }
    return *this;
}

ProcessBuilder& ProcessBuilder::clearArguments() {
    config_.arguments.clear();
    return *this;
}

ProcessBuilder& ProcessBuilder::clearEnvironment() {
    config_.environment.clear();
    return *this;
}

Pipe ProcessBuilder::captureStdout() {
    return makeCapturedPipe(config_.stdoutRedirect, stdoutCtrl_, parentCloseFds_, false);
}

Pipe ProcessBuilder::captureStderr() {
    return makeCapturedPipe(config_.stderrRedirect, stderrCtrl_, parentCloseFds_, false);
}

Pipe ProcessBuilder::captureStdin() {
    return makeCapturedPipe(config_.stdinRedirect, stdinCtrl_, parentCloseFds_, true);
}

ProcessHandle ProcessBuilder::spawn() {
    ProcessConfig configCopy = config_;
    configCopy.parentCloseFds = parentCloseFds_;
    auto handle = launchProcess(configCopy);

    auto cleanup = [](RedirectControl& ctrl, ProcessConfig::FdRedirect& redirect) {
        if (ctrl.closeParentAfterSpawn) {
            if (redirect.fd) {
                ::close(*redirect.fd);
                redirect.fd.reset();
            }
            ctrl.closeParentAfterSpawn = false;
        }
    };

    cleanup(stdinCtrl_, config_.stdinRedirect);
    cleanup(stdoutCtrl_, config_.stdoutRedirect);
    cleanup(stderrCtrl_, config_.stderrRedirect);

    parentCloseFds_.clear();
    return handle;
}

}  // namespace pman
