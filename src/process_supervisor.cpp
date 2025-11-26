#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "pman/process_supervisor.hpp"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace pman {

ProcessSupervisor::ProcessSupervisor() {
}

ProcessSupervisor::~ProcessSupervisor() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
    if (signalFd_ >= 0) {
        ::close(signalFd_);
    }
}

void ProcessSupervisor::add(SupervisedProcess spec) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (processes_.count(spec.name)) {
        throw std::runtime_error("Process already exists: " + spec.name);
    }

    ProcessState state;
    state.spec = std::move(spec);
    state.status.name = state.spec.name;

    processes_[state.spec.name] = std::move(state);
}

void ProcessSupervisor::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = processes_.find(name);
    if (it == processes_.end()) {
        return;
    }

    if (it->second.handle && it->second.handle->valid()) {
        try {
            it->second.handle->terminate();
        } catch (const std::system_error& e) {
            if (e.code().value() != ESRCH) {
                throw;
            }
        }
        int status = 0;
        it->second.handle->waitFor(std::chrono::seconds(5), status);
        if (it->second.handle->valid()) {
            try {
                it->second.handle->kill();
                it->second.handle->wait();
            } catch (const std::system_error& e) {
                if (e.code().value() != ESRCH) {
                    throw;
                }
            }
        }
        pidToName_.erase(it->second.handle->pid());
    }

    processes_.erase(it);
}

void ProcessSupervisor::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);

    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        throw std::system_error(errno, std::generic_category(), "pthread_sigmask");
    }

    signalFd_ = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signalFd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "signalfd");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, state] : processes_) {
            doStartProcess(state);
        }
    }

    running_.store(true, std::memory_order_release);

    supervisorThread_ = std::make_unique<ManagedThread>(
        "pman-supervisor",
        [this]() { supervisorLoop(); });
}

void ProcessSupervisor::stop() {
    running_.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, state] : processes_) {
            if (state.handle && state.handle->valid()) {
                try {
                    state.handle->kill();
                } catch (const std::system_error& e) {
                    // Ignore ESRCH (process already exited) - it's what we wanted anyway
                    if (e.code().value() != ESRCH) {
                        throw;
                    }
                }
            }
        }
    }

    if (supervisorThread_ && supervisorThread_->joinable()) {
        supervisorThread_->join();
    }
    supervisorThread_.reset();

    if (signalFd_ >= 0) {
        ::close(signalFd_);
        signalFd_ = -1;
    }
}

void ProcessSupervisor::stopGracefully(std::chrono::nanoseconds timeout) {
    running_.store(false, std::memory_order_release);

    auto deadline = std::chrono::steady_clock::now() + timeout;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, state] : processes_) {
            if (state.handle && state.handle->valid()) {
                try {
                    state.handle->terminate();
                } catch (const std::system_error& e) {
                    // Ignore ESRCH (process already exited)
                    if (e.code().value() != ESRCH) {
                        throw;
                    }
                }
            }
        }
    }

    while (std::chrono::steady_clock::now() < deadline) {
        bool allStopped = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [name, state] : processes_) {
                if (state.handle && state.handle->valid()) {
                    int status = 0;
                    if (state.handle->tryWait(status)) {
                        state.status.running = false;
                        state.status.lastExitStatus = status;
                    } else {
                        allStopped = false;
                    }
                }
            }
        }
        if (allStopped) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, state] : processes_) {
            if (state.handle && state.handle->valid()) {
                try {
                    state.handle->kill();
                } catch (const std::system_error& e) {
                    // Ignore ESRCH (process already exited)
                    if (e.code().value() != ESRCH) {
                        throw;
                    }
                }
            }
        }
    }

    if (supervisorThread_ && supervisorThread_->joinable()) {
        supervisorThread_->join();
    }
    supervisorThread_.reset();

    if (signalFd_ >= 0) {
        ::close(signalFd_);
        signalFd_ = -1;
    }
}

void ProcessSupervisor::startProcess(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = processes_.find(name);
    if (it == processes_.end()) {
        throw std::runtime_error("Unknown process: " + name);
    }

    if (it->second.handle && it->second.handle->valid()) {
        return;
    }

    doStartProcess(it->second);
}

void ProcessSupervisor::stopProcess(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = processes_.find(name);
    if (it == processes_.end()) {
        throw std::runtime_error("Unknown process: " + name);
    }

    it->second.pendingRestart = false;

    if (it->second.handle && it->second.handle->valid()) {
        pidToName_.erase(it->second.handle->pid());
        try {
            it->second.handle->terminate();
        } catch (const std::system_error& e) {
            if (e.code().value() != ESRCH) {
                throw;
            }
        }
    }
}

void ProcessSupervisor::restartProcess(const std::string& name) {
    stopProcess(name);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = processes_.find(name);
    if (it != processes_.end()) {
        it->second.status.restartCount = 0;
        it->second.restartTimes.clear();
        doStartProcess(it->second);
    }
}

std::vector<ProcessStatus> ProcessSupervisor::status() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ProcessStatus> result;
    result.reserve(processes_.size());

    for (const auto& [name, state] : processes_) {
        result.push_back(state.status);
    }

    return result;
}

ProcessStatus ProcessSupervisor::status(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = processes_.find(name);
    if (it == processes_.end()) {
        throw std::runtime_error("Unknown process: " + name);
    }

    return it->second.status;
}

void ProcessSupervisor::onProcessExit(std::function<void(const std::string&, int)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    exitCallback_ = std::move(callback);
}

void ProcessSupervisor::onProcessStart(std::function<void(const std::string&, pid_t)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    startCallback_ = std::move(callback);
}

void ProcessSupervisor::supervisorLoop() {
    while (running_.load(std::memory_order_acquire)) {
        pollfd pfd{};
        pfd.fd = signalFd_;
        pfd.events = POLLIN;

        int rc = ::poll(&pfd, 1, 100);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (rc > 0 && (pfd.revents & POLLIN)) {
            signalfd_siginfo info;
            ssize_t n = ::read(signalFd_, &info, sizeof(info));
            if (n == sizeof(info)) {
                int status = 0;
                pid_t pid;
                while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
                    handleChildExit(pid, status);
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, state] : processes_) {
            if (state.pendingRestart && now >= state.scheduledRestartTime) {
                state.pendingRestart = false;
                doStartProcess(state);
            }
        }
    }

    int status = 0;
    pid_t pid;
    while ((pid = ::waitpid(-1, &status, WNOHANG)) > 0) {
    }
}

void ProcessSupervisor::handleChildExit(pid_t pid, int status) {
    std::string name;
    std::function<void(const std::string&, int)> exitCb;
    std::function<void(int)> specExitCb;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = pidToName_.find(pid);
        if (it == pidToName_.end()) {
            return;
        }

        name = it->second;
        pidToName_.erase(it);

        auto procIt = processes_.find(name);
        if (procIt == processes_.end()) {
            return;
        }

        ProcessState& state = procIt->second;
        state.status.running = false;
        state.status.lastExitStatus = status;
        state.status.lastExitTime = std::chrono::steady_clock::now();
        state.status.pid = -1;

        if (state.spec.onExit) {
            specExitCb = state.spec.onExit;
        }
        exitCb = exitCallback_;

        if (running_.load(std::memory_order_acquire) && shouldRestart(name, status)) {
            auto delay = calculateRestartDelay(name);
            state.status.nextRestartDelay = delay;
            state.pendingRestart = true;
            state.scheduledRestartTime = std::chrono::steady_clock::now() + delay;
            state.status.restartCount++;
            state.restartTimes.push_back(std::chrono::steady_clock::now());
        }
    }

    if (specExitCb) {
        try {
            specExitCb(status);
        } catch (...) {}
    }

    if (exitCb) {
        try {
            exitCb(name, status);
        } catch (...) {}
    }
}

bool ProcessSupervisor::shouldRestart(const std::string& name, int status) {
    auto it = processes_.find(name);
    if (it == processes_.end()) {
        return false;
    }

    const RestartConfig& rc = it->second.spec.restart;
    ProcessState& state = it->second;

    cleanupRestartHistory(state);

    if (rc.maxRestarts >= 0 && state.status.restartCount >= rc.maxRestarts) {
        return false;
    }

    if (static_cast<int>(state.restartTimes.size()) >= rc.maxRestartsInWindow) {
        return false;
    }

    switch (rc.policy) {
    case RestartPolicy::Never:
        return false;
    case RestartPolicy::Always:
        return true;
    case RestartPolicy::OnFailure:
        return !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    case RestartPolicy::OnSuccess:
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    return false;
}

std::chrono::nanoseconds ProcessSupervisor::calculateRestartDelay(const std::string& name) {
    auto it = processes_.find(name);
    if (it == processes_.end()) {
        return std::chrono::seconds(1);
    }

    const RestartConfig& rc = it->second.spec.restart;
    int restartCount = it->second.status.restartCount;

    std::chrono::nanoseconds delay;

    switch (rc.backoffStrategy) {
    case BackoffStrategy::Exponential: {
        double multiplier = 1.0;
        for (int i = 0; i < restartCount && i < 32; ++i) {
            multiplier *= rc.backoffMultiplier;
        }
        delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double, std::nano>(
                rc.minRestartDelay.count() * multiplier));
        break;
    }
    case BackoffStrategy::Linear:
        delay = rc.minRestartDelay + (rc.linearIncrement * restartCount);
        break;
    case BackoffStrategy::Custom:
        if (rc.customBackoff) {
            delay = rc.customBackoff(restartCount);
        } else {
            delay = rc.minRestartDelay;
        }
        break;
    }

    if (delay > rc.maxRestartDelay) {
        delay = rc.maxRestartDelay;
    }
    if (delay < rc.minRestartDelay) {
        delay = rc.minRestartDelay;
    }

    return delay;
}

void ProcessSupervisor::doStartProcess(ProcessState& state) {
    try {
        state.handle = launchProcess(state.spec.config);
        state.status.pid = state.handle->pid();
        state.status.running = true;
        state.status.lastStartTime = std::chrono::steady_clock::now();

        pidToName_[state.handle->pid()] = state.spec.name;

        if (state.spec.onRestart && state.status.restartCount > 0) {
            try {
                state.spec.onRestart();
            } catch (...) {}
        }

        if (startCallback_) {
            try {
                startCallback_(state.spec.name, state.handle->pid());
            } catch (...) {}
        }
    } catch (const std::exception&) {
        state.status.running = false;
        state.status.pid = -1;
        throw;
    }
}

void ProcessSupervisor::cleanupRestartHistory(ProcessState& state) {
    auto now = std::chrono::steady_clock::now();
    auto windowStart = now - state.spec.restart.maxRestartWindow;

    state.restartTimes.erase(
        std::remove_if(state.restartTimes.begin(), state.restartTimes.end(),
            [windowStart](const auto& t) { return t < windowStart; }),
        state.restartTimes.end());
}

}  // namespace pman
