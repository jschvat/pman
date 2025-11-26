#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "pman/process.hpp"

#include "pman/diagnostics.hpp"
#include "pman/topology.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace pman {

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void throwSystemError(int err, const char* context) {
    throw std::system_error(err, std::generic_category(), context);
}

std::vector<std::string> buildArgumentVector(const ProcessConfig& config) {
    std::vector<std::string> args;
    args.reserve(1 + config.arguments.size());
    args.push_back(config.executable);
    args.insert(args.end(), config.arguments.begin(), config.arguments.end());
    return args;
}

std::vector<char*> toCharPtrs(std::vector<std::string>& storage) {
    std::vector<char*> result;
    result.reserve(storage.size() + 1);
    for (auto& entry : storage) {
        result.push_back(entry.data());
    }
    result.push_back(nullptr);
    return result;
}

std::vector<std::string> buildEnvironment(const ProcessConfig& config) {
    std::vector<std::string> env;
    if (config.inheritEnvironment && environ) {
        for (char** current = environ; *current != nullptr; ++current) {
            env.emplace_back(*current);
        }
    }
    env.insert(env.end(), config.environment.begin(), config.environment.end());
    return env;
}

int openPidfd(pid_t pid) {
#ifdef SYS_pidfd_open
    int fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    return fd;
#else
    (void)pid;
    return -1;
#endif
}

void applyRedirect(const ProcessConfig::FdRedirect& redirect, int targetFd) {
    if (!redirect.fd) {
        return;
    }
    if (*redirect.fd != targetFd) {
        if (::dup2(*redirect.fd, targetFd) < 0) {
            _exit(126);
        }
    }
    if (redirect.closeAfterDup && *redirect.fd != targetFd) {
        ::close(*redirect.fd);
    }
}

bool canUsePosixSpawn(const ProcessConfig& config) {
    if (!config.usePosixSpawn) {
        return false;
    }
    if (config.workingDirectory || config.newSession || config.parentDeathSignal || config.niceValue ||
        config.cgroupPath || config.ioniceClass) {
        return false;
    }
    return true;
}

void applyPostSpawnParentWork(const ProcessConfig& config, pid_t pid, const CpuSet* affinity) {
#ifdef __linux__
    if (affinity) {
        ::sched_setaffinity(pid, sizeof(cpu_set_t), affinity->data());
    }
#endif
    for (int fd : config.parentCloseFds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
}

ProcessHandle finalizeHandle(const ProcessConfig& config, pid_t pid, int pidfd) {
    ProcessHandle handle(pid, config.executable, pidfd);
    if (config.cgroupPath) {
        handle.attachToCgroup(*config.cgroupPath);
    }
    if (config.ioniceClass) {
        handle.setIoPriority(*config.ioniceClass, config.ionicePriority.value_or(0));
    }
    return handle;
}

ProcessHandle launchWithPosixSpawn(const ProcessConfig& config,
                                   std::vector<char*>& argv,
                                   std::vector<char*>& envp,
                                   const CpuSet* affinity) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);

    auto addRedirect = [&](const ProcessConfig::FdRedirect& redirect, int targetFd) {
        if (!redirect.fd) {
            return;
        }
        posix_spawn_file_actions_adddup2(&actions, *redirect.fd, targetFd);
        if (redirect.closeAfterDup) {
            posix_spawn_file_actions_addclose(&actions, *redirect.fd);
        }
    };

    addRedirect(config.stdinRedirect, STDIN_FILENO);
    addRedirect(config.stdoutRedirect, STDOUT_FILENO);
    addRedirect(config.stderrRedirect, STDERR_FILENO);

    pid_t pid = -1;
    int rc = config.searchPath ? posix_spawnp(&pid, config.executable.c_str(), &actions, nullptr, argv.data(), envp.data())
                               : posix_spawn(&pid, config.executable.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0) {
        detail::emitProcessEvent(ProcessEvent{
            .type = ProcessEventType::LaunchFailure,
            .pid = -1,
            .executable = config.executable,
            .error = rc,
            .timestamp = Clock::now(),
            .message = "posix_spawn failed",
        });
        throw std::system_error(rc, std::generic_category(), "posix_spawn");
    }

    detail::emitProcessEvent(ProcessEvent{
        .type = ProcessEventType::LaunchSuccess,
        .pid = pid,
        .executable = config.executable,
        .timestamp = Clock::now(),
    });

    applyPostSpawnParentWork(config, pid, affinity);
    int pidfd = openPidfd(pid);
    return finalizeHandle(config, pid, pidfd);
}

}  // namespace

void ProcessHandle::sendSignal(int signal) const {
    if (!valid()) {
        throw std::logic_error("ProcessHandle is not valid");
    }
    if (::kill(pid_, signal) != 0) {
        throwSystemError(errno, "kill");
    }
}

void ProcessHandle::terminate() const {
    sendSignal(SIGTERM);
}

void ProcessHandle::kill() const {
    sendSignal(SIGKILL);
}

int ProcessHandle::wait() {
    if (!valid()) {
        throw std::logic_error("ProcessHandle is not valid");
    }

    int status = 0;
    pid_t rc = ::waitpid(pid_, &status, 0);
    if (rc < 0) {
        throwSystemError(errno, "waitpid");
    }
    detail::emitProcessEvent(ProcessEvent{
        .type = ProcessEventType::Exit,
        .pid = pid_,
        .executable = executable_,
        .status = status,
        .timestamp = Clock::now(),
    });
    closePidfd();
    pid_ = -1;
    return status;
}

bool ProcessHandle::tryWait(int& status) {
    if (!valid()) {
        return false;
    }
    pid_t rc = ::waitpid(pid_, &status, WNOHANG);
    if (rc < 0) {
        throwSystemError(errno, "waitpid");
    }
    if (rc == pid_) {
        detail::emitProcessEvent(ProcessEvent{
            .type = ProcessEventType::Exit,
            .pid = pid_,
            .executable = executable_,
            .status = status,
            .timestamp = Clock::now(),
        });
        closePidfd();
        pid_ = -1;
        return true;
    }
    return false;
}

bool ProcessHandle::waitFor(std::chrono::nanoseconds timeout) {
    int status = 0;
    return waitFor(timeout, status);
}

bool ProcessHandle::waitFor(std::chrono::nanoseconds timeout, int& status) {
    if (!valid()) {
        return true;
    }

    if (pidfd_ >= 0) {
        pollfd pfd{};
        pfd.fd = pidfd_;
        pfd.events = POLLIN;
        int timeoutMs = -1;
        if (timeout.count() >= 0) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
            if (ms.count() > std::numeric_limits<int>::max()) {
                timeoutMs = std::numeric_limits<int>::max();
            } else {
                timeoutMs = static_cast<int>(ms.count());
            }
        }
        int rc = ::poll(&pfd, 1, timeoutMs);
        if (rc <= 0) {
            return false;
        }
        status = wait();
        return true;
    }

    auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (tryWait(status)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool ProcessHandle::terminate(std::chrono::nanoseconds gracePeriod) {
    if (!valid()) {
        return true;
    }
    sendSignal(SIGTERM);
    int status = 0;
    if (waitFor(gracePeriod, status)) {
        return true;
    }
    kill();
    wait();
    return false;
}

void ProcessHandle::setCpuAffinity(const CpuSet& set) const {
#ifdef __linux__
    if (!valid()) {
        throw std::logic_error("ProcessHandle is not valid");
    }
    int rc = ::sched_setaffinity(pid_, sizeof(cpu_set_t), set.data());
    if (rc != 0) {
        throwSystemError(errno, "sched_setaffinity");
    }
#else
    (void)set;
    throw std::runtime_error("sched_setaffinity unavailable on this platform");
#endif
}

void ProcessHandle::setNiceValue(int value) const {
    if (!valid()) {
        throw std::logic_error("ProcessHandle is not valid");
    }
    if (::setpriority(PRIO_PROCESS, pid_, value) != 0) {
        throwSystemError(errno, "setpriority");
    }
}

void ProcessHandle::attachToCgroup(const std::string& path) const {
    if (!valid()) {
        throw std::logic_error("ProcessHandle is not valid");
    }
    std::ofstream file(path + "/cgroup.procs");
    if (!file.is_open()) {
        throw std::runtime_error("failed to open cgroup: " + path);
    }
    file << pid_;
    if (!file) {
        throw std::runtime_error("failed to write pid to cgroup");
    }
}

void ProcessHandle::setIoPriority(int klass, int priority) const {
#ifdef __linux__
    if (!valid()) {
        throw std::logic_error("ProcessHandle is not valid");
    }
    int value = (klass << 13) | (priority & 0xfff);
    if (::syscall(SYS_ioprio_set, 1, pid_, value) != 0) {
        throwSystemError(errno, "ioprio_set");
    }
#else
    (void)klass;
    (void)priority;
    throw std::runtime_error("ionice unsupported on this platform");
#endif
}

ProcessHandle::~ProcessHandle() {
    closePidfd();
}

ProcessHandle::ProcessHandle(ProcessHandle&& other) noexcept {
    *this = std::move(other);
}

ProcessHandle& ProcessHandle::operator=(ProcessHandle&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    closePidfd();
    pid_ = other.pid_;
    executable_ = std::move(other.executable_);
    pidfd_ = other.pidfd_;
    other.pid_ = -1;
    other.pidfd_ = -1;
    other.executable_.clear();
    return *this;
}

void ProcessHandle::closePidfd() {
    if (pidfd_ >= 0) {
        ::close(pidfd_);
        pidfd_ = -1;
    }
}

ProcessHandle launchProcess(const ProcessConfig& config) {
    if (config.executable.empty()) {
        throw std::invalid_argument("executable path is required");
    }

    CpuSet derivedAffinity;
    const CpuSet* affinity = nullptr;
    if (config.cpuAffinity) {
        affinity = &*config.cpuAffinity;
    } else if (config.numaNode) {
        try {
            derivedAffinity = SystemTopology::instance().cpuSetForNode(*config.numaNode);
            affinity = &derivedAffinity;
        } catch (...) {
            affinity = nullptr;
        }
    }

    detail::emitProcessEvent(ProcessEvent{
        .type = ProcessEventType::LaunchAttempt,
        .pid = -1,
        .executable = config.executable,
        .timestamp = Clock::now(),
    });

    std::vector<std::string> argStorage = buildArgumentVector(config);
    std::vector<char*> argv = toCharPtrs(argStorage);

    std::vector<std::string> envStorage = buildEnvironment(config);
    std::vector<char*> envp = toCharPtrs(envStorage);

    if (canUsePosixSpawn(config)) {
        return launchWithPosixSpawn(config, argv, envp, affinity);
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        detail::emitProcessEvent(ProcessEvent{
            .type = ProcessEventType::LaunchFailure,
            .pid = -1,
            .executable = config.executable,
            .error = errno,
            .timestamp = Clock::now(),
            .message = "fork failed",
        });
        throwSystemError(errno, "fork");
    }

    if (pid == 0) {
        if (config.workingDirectory) {
            if (::chdir(config.workingDirectory->c_str()) != 0) {
                _exit(126);
            }
        }

        if (config.newSession) {
            ::setsid();
        }

        if (config.parentDeathSignal) {
            ::prctl(PR_SET_PDEATHSIG, *config.parentDeathSignal);
        }

        if (config.niceValue) {
            ::setpriority(PRIO_PROCESS, 0, *config.niceValue);
        }

        applyRedirect(config.stdinRedirect, STDIN_FILENO);
        applyRedirect(config.stdoutRedirect, STDOUT_FILENO);
        applyRedirect(config.stderrRedirect, STDERR_FILENO);

        if (config.searchPath) {
            ::execvpe(config.executable.c_str(), argv.data(), envp.data());
        } else {
            ::execve(config.executable.c_str(), argv.data(), envp.data());
        }
        _exit(127);
    }

    detail::emitProcessEvent(ProcessEvent{
        .type = ProcessEventType::LaunchSuccess,
        .pid = pid,
        .executable = config.executable,
        .timestamp = Clock::now(),
    });

    applyPostSpawnParentWork(config, pid, affinity);
    int pidfd = openPidfd(pid);

    return finalizeHandle(config, pid, pidfd);
}

}  // namespace pman
