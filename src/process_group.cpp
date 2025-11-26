#include "pman/process_group.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pman {

ProcessGroup::~ProcessGroup() {
}

ProcessGroup::ProcessGroup(ProcessGroup&& other) noexcept
    : processes_(std::move(other.processes_)),
      pgid_(other.pgid_) {
    other.pgid_.reset();
}

ProcessGroup& ProcessGroup::operator=(ProcessGroup&& other) noexcept {
    if (this != &other) {
        processes_ = std::move(other.processes_);
        pgid_ = other.pgid_;
        other.pgid_.reset();
    }
    return *this;
}

ProcessHandle& ProcessGroup::add(ProcessConfig config) {
    ProcessHandle handle = launchProcess(config);

    if (pgid_) {
        if (::setpgid(handle.pid(), *pgid_) != 0 && errno != EACCES && errno != ESRCH) {
            throw std::system_error(errno, std::generic_category(), "setpgid");
        }
    }

    processes_.push_back(std::move(handle));
    return processes_.back();
}

ProcessHandle& ProcessGroup::add(ProcessBuilder&& builder) {
    ProcessHandle handle = builder.spawn();

    if (pgid_) {
        if (::setpgid(handle.pid(), *pgid_) != 0 && errno != EACCES && errno != ESRCH) {
            throw std::system_error(errno, std::generic_category(), "setpgid");
        }
    }

    processes_.push_back(std::move(handle));
    return processes_.back();
}

void ProcessGroup::sendSignalAll(int signal) {
    if (pgid_) {
        if (::killpg(*pgid_, signal) != 0 && errno != ESRCH) {
            throw std::system_error(errno, std::generic_category(), "killpg");
        }
    } else {
        for (auto& proc : processes_) {
            if (proc.valid()) {
                try {
                    proc.sendSignal(signal);
                } catch (const std::system_error& e) {
                    if (e.code().value() != ESRCH) {
                        throw;
                    }
                }
            }
        }
    }
}

void ProcessGroup::terminateAll() {
    sendSignalAll(SIGTERM);
}

void ProcessGroup::killAll() {
    sendSignalAll(SIGKILL);
}

void ProcessGroup::stopAll() {
    sendSignalAll(SIGSTOP);
}

void ProcessGroup::continueAll() {
    sendSignalAll(SIGCONT);
}

int ProcessGroup::waitAny() {
    if (processes_.empty()) {
        throw std::runtime_error("ProcessGroup is empty");
    }

    int status = 0;
    pid_t pid = ::waitpid(-1, &status, 0);
    if (pid < 0) {
        throw std::system_error(errno, std::generic_category(), "waitpid");
    }

    for (std::size_t i = 0; i < processes_.size(); ++i) {
        if (processes_[i].valid() && processes_[i].pid() == pid) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

std::vector<int> ProcessGroup::waitAll() {
    std::vector<int> statuses;
    statuses.reserve(processes_.size());

    for (auto& proc : processes_) {
        if (proc.valid()) {
            statuses.push_back(proc.wait());
        } else {
            statuses.push_back(-1);
        }
    }

    return statuses;
}

bool ProcessGroup::waitAllFor(std::chrono::nanoseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;

    for (auto& proc : processes_) {
        if (proc.valid()) {
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::nanoseconds::zero()) {
                return false;
            }
            if (!proc.waitFor(remaining)) {
                return false;
            }
        }
    }

    return true;
}

std::size_t ProcessGroup::size() const noexcept {
    return processes_.size();
}

bool ProcessGroup::empty() const noexcept {
    return processes_.empty();
}

ProcessHandle& ProcessGroup::at(std::size_t index) {
    return processes_.at(index);
}

const ProcessHandle& ProcessGroup::at(std::size_t index) const {
    return processes_.at(index);
}

ProcessHandle& ProcessGroup::operator[](std::size_t index) {
    return processes_[index];
}

const ProcessHandle& ProcessGroup::operator[](std::size_t index) const {
    return processes_[index];
}

void ProcessGroup::setProcessGroup(pid_t pgid) {
    pgid_ = pgid;

    for (auto& proc : processes_) {
        if (proc.valid()) {
            if (::setpgid(proc.pid(), pgid) != 0 && errno != EACCES && errno != ESRCH) {
                throw std::system_error(errno, std::generic_category(), "setpgid");
            }
        }
    }
}

void ProcessGroup::createProcessGroup() {
    if (processes_.empty()) {
        throw std::runtime_error("Cannot create process group: no processes");
    }

    pid_t leader = processes_[0].pid();
    if (::setpgid(leader, 0) != 0 && errno != EACCES && errno != ESRCH) {
        throw std::system_error(errno, std::generic_category(), "setpgid");
    }

    pgid_ = leader;

    for (std::size_t i = 1; i < processes_.size(); ++i) {
        if (processes_[i].valid()) {
            if (::setpgid(processes_[i].pid(), leader) != 0 && errno != EACCES && errno != ESRCH) {
                throw std::system_error(errno, std::generic_category(), "setpgid");
            }
        }
    }
}

}  // namespace pman
