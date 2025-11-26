#include "pman/signal.hpp"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <stdexcept>
#include <system_error>
#include <thread>

namespace pman {
namespace {

void blockSignals(const sigset_t& mask) {
    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        throw std::system_error(errno, std::generic_category(), "pthread_sigmask");
    }
}

sigset_t buildMask(const std::vector<int>& signals) {
    sigset_t mask;
    sigemptyset(&mask);
    for (int sig : signals) {
        sigaddset(&mask, sig);
    }
    return mask;
}

}  // namespace

SignalDispatcher& SignalDispatcher::instance() {
    static SignalDispatcher dispatcher;
    return dispatcher;
}

SignalDispatcher::~SignalDispatcher() {
    stop();
}

void SignalDispatcher::start(const std::vector<int>& signals) {
#ifdef __linux__
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }
    if (signals.empty()) {
        throw std::invalid_argument("SignalDispatcher requires at least one signal");
    }
    mask_ = buildMask(signals);
    blockSignals(mask_);

    fd_ = signalfd(-1, &mask_, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "signalfd");
    }
    running_ = true;
    thread_ = std::thread([this]() { run(); });
#else
    (void)signals;
    throw std::runtime_error("SignalDispatcher requires signalfd (Linux only)");
#endif
}

void SignalDispatcher::stop() {
#ifdef __linux__
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.clear();
#else
    // no-op
#endif
}

void SignalDispatcher::registerHandler(int signal, SignalHandler handler) {
#ifdef __linux__
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_[signal] = std::move(handler);
#else
    (void)signal;
    (void)handler;
#endif
}

void SignalDispatcher::unregisterHandler(int signal) {
#ifdef __linux__
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.erase(signal);
#else
    (void)signal;
#endif
}

void SignalDispatcher::blockSignalsForCurrentThread() {
#ifdef __linux__
    blockSignals(mask_);
#endif
}

void SignalDispatcher::run() {
#ifdef __linux__
    while (true) {
        signalfd_siginfo info;
        ssize_t bytes = ::read(fd_, &info, sizeof(info));
        if (bytes == 0) {
            break;
        }
        if (bytes < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            break;
        }

        SignalHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                break;
            }
            auto it = handlers_.find(static_cast<int>(info.ssi_signo));
            if (it != handlers_.end()) {
                handler = it->second;
            }
        }
        if (handler) {
            handler(info);
        }
    }
#endif
}

void blockSignalsInCurrentThread(const std::vector<int>& signals) {
#ifdef __linux__
    sigset_t mask = buildMask(signals);
    blockSignals(mask);
#else
    (void)signals;
#endif
}

bool sendSignalSequence(pid_t target,
                        const std::vector<std::pair<int, std::chrono::nanoseconds>>& sequence,
                        bool processGroup) {
    for (const auto& entry : sequence) {
        int sig = entry.first;
        auto delay = entry.second;
        pid_t recipient = processGroup ? -target : target;
        if (::kill(recipient, sig) != 0) {
            if (errno == ESRCH) {
                return true;
            }
            return false;
        }
        if (delay.count() > 0) {
            std::this_thread::sleep_for(delay);
        }
    }
    return true;
}

bool gracefulShutdown(pid_t pid, const GracefulShutdownOptions& options) {
    std::vector<std::pair<int, std::chrono::nanoseconds>> sequence;
    sequence.emplace_back(SIGTERM, options.termTimeout);
    if (options.sendInt) {
        sequence.emplace_back(SIGINT, options.intTimeout);
    }
    if (options.sendHup) {
        sequence.emplace_back(SIGHUP, options.hupTimeout);
    }
    sequence.emplace_back(SIGKILL, std::chrono::nanoseconds(0));
    return sendSignalSequence(pid, sequence, options.processGroup);
}

}  // namespace pman
