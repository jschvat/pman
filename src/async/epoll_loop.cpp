#include "epoll_loop.hpp"
#include "pman/async/task.hpp"
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace pman::async {

EpollLoop::EpollLoop(const EventLoopConfig& config) {
    epollFd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd_ < 0) {
        throw std::runtime_error("Failed to create epoll fd");
    }

    eventFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd_ < 0) {
        close(epollFd_);
        throw std::runtime_error("Failed to create event fd");
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = eventFd_;
    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, eventFd_, &ev) < 0) {
        close(eventFd_);
        close(epollFd_);
        throw std::runtime_error("Failed to add eventfd to epoll");
    }
}

EpollLoop::~EpollLoop() {
    if (eventFd_ >= 0) {
        close(eventFd_);
    }
    if (epollFd_ >= 0) {
        close(epollFd_);
    }
}

void EpollLoop::run() {
    running_ = true;

    std::vector<struct epoll_event> events(64);

    while (running_) {
        auto timeout = getNextTimerTimeout();
        int timeoutMs = timeout.count();

        // DEBUG
        //std::cerr << "[EPOLL] Loop iteration: running=" << running_ << ", timeout=" << timeoutMs << "ms, timers=" << activeTimers() << "\n" << std::flush;

        int nfds = epoll_wait(epollFd_, events.data(), events.size(), timeoutMs);

        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("epoll_wait failed");
        }

        processTimers();

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            uint32_t revents = events[i].events;

            if (fd == eventFd_) {
                uint64_t val;
                read(eventFd_, &val, sizeof(val));
                continue;
            }

            auto it = fdCallbacks_.find(fd);
            if (it != fdCallbacks_.end()) {
                detail::TrampolineGuard guard;
                Event event = static_cast<Event>(revents);
                it->second(event);
            }
        }

        // Process posted callbacks at end of iteration
        processPostedCallbacks();
    }
}

void EpollLoop::stop() {
    running_ = false;
    uint64_t val = 1;
    write(eventFd_, &val, sizeof(val));
}

uint64_t EpollLoop::addTimer(std::chrono::nanoseconds duration, std::function<void()> callback) {
    uint64_t id = nextTimerId_++;
    auto expiry = std::chrono::steady_clock::now() + duration;

    TimerData timer{id, expiry};
    timers_.push(timer);
    timerCallbacks_[id] = std::move(callback);
    timerPeriods_[id] = std::chrono::nanoseconds(0);

    // OPTIMIZATION: No eventfd write needed - runOnce() calculates timeout dynamically

    return id;
}

uint64_t EpollLoop::addPeriodicTimer(std::chrono::nanoseconds period, std::function<void()> callback) {
    uint64_t id = nextTimerId_++;
    auto expiry = std::chrono::steady_clock::now() + period;

    TimerData timer{id, expiry};
    timers_.push(timer);
    timerCallbacks_[id] = std::move(callback);
    timerPeriods_[id] = period;

    // OPTIMIZATION: No eventfd write needed - runOnce() calculates timeout dynamically

    return id;
}

bool EpollLoop::cancelTimer(uint64_t timerId) {
    auto erased1 = timerCallbacks_.erase(timerId);
    timerPeriods_.erase(timerId);
    return erased1 > 0;
}

bool EpollLoop::runOnce(std::chrono::nanoseconds timeout) {
    auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
    struct epoll_event events[64];

    int nfds = epoll_wait(epollFd_, events, 64, timeout_ms);
    if (nfds < 0 && errno != EINTR) {
        return false;
    }

    // OPTIMIZATION: Only process timers if we have any and they're ready
    // Check AFTER epoll_wait since time has passed
    if (!timers_.empty()) {
        auto now = std::chrono::steady_clock::now();
        if (timers_.top().expiry <= now) {
            processTimers();
        }
    }

    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;
        uint32_t revents = events[i].events;

        if (fd == eventFd_) {
            uint64_t val;
            read(eventFd_, &val, sizeof(val));
            continue;
        }

        auto it = fdCallbacks_.find(fd);
        if (it != fdCallbacks_.end()) {
            detail::TrampolineGuard guard;
            Event event = static_cast<Event>(revents);
            it->second(event);
        }
    }

    // CRITICAL: Process posted callbacks at end of iteration
    processPostedCallbacks();

    return nfds > 0;
}

bool EpollLoop::isRunning() const noexcept {
    return running_;
}

BackendType EpollLoop::backend() const noexcept {
    return BackendType::Epoll;
}

void EpollLoop::addFd(int fd, Event events, EventCallback callback) {
    struct epoll_event ev;
    ev.events = static_cast<uint32_t>(events);
    ev.data.fd = fd;

    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        throw std::runtime_error("Failed to add fd to epoll");
    }

    fdCallbacks_[fd] = std::move(callback);
}

void EpollLoop::modifyFd(int fd, Event events) {
    struct epoll_event ev;
    ev.events = static_cast<uint32_t>(events);
    ev.data.fd = fd;
    epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev);
}

void EpollLoop::removeFd(int fd) {
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    fdCallbacks_.erase(fd);
}

void EpollLoop::post(std::function<void()> callback) {
    postedCallbacks_.push_back(std::move(callback));
    // Wake up the event loop
    uint64_t val = 1;
    write(eventFd_, &val, sizeof(val));
}

void EpollLoop::processPostedCallbacks() {
    if (postedCallbacks_.empty()) {
        return;
    }

    // Move callbacks to local vector to avoid issues if callbacks post more callbacks
    std::vector<std::function<void()>> callbacks;
    callbacks.swap(postedCallbacks_);

    for (auto& callback : callbacks) {
        if (callback) {
            detail::TrampolineGuard guard;
            callback();
        }
    }
}

size_t EpollLoop::activeFds() const noexcept {
    return fdCallbacks_.size();
}

size_t EpollLoop::activeTimers() const noexcept {
    return timerCallbacks_.size();
}

void EpollLoop::processTimers() {
    auto now = std::chrono::steady_clock::now();

    // OPTIMIZATION: Batch callbacks for better throughput
    std::vector<std::function<void()>> batch;
    batch.reserve(32);  // Reserve space for typical batch size

    while (!timers_.empty()) {
        const auto& timer = timers_.top();

        if (timer.expiry > now) {
            break;
        }

        // CRITICAL FIX: Copy timer ID before popping! The reference becomes invalid after pop().
        uint64_t timerId = timer.id;

        auto callbackIt = timerCallbacks_.find(timerId);
        auto periodIt = timerPeriods_.find(timerId);

        if (callbackIt == timerCallbacks_.end() || periodIt == timerPeriods_.end()) {
            timers_.pop();
            continue;
        }

        // CRITICAL FIX: Copy callback BEFORE any map modifications
        auto callback = callbackIt->second;
        auto period = periodIt->second;

        timers_.pop();

        // Reschedule periodic timer or cleanup one-shot
        if (period.count() > 0) {
            TimerData nextTimer{timerId, now + period};
            timers_.push(nextTimer);
        } else {
            timerCallbacks_.erase(timerId);
            timerPeriods_.erase(timerId);
        }

        // OPTIMIZATION: Batch callback instead of executing immediately
        if (callback) {
            batch.push_back(std::move(callback));
        }
    }

    // Execute all callbacks in batch (better cache locality, fewer context switches)
    detail::TrampolineGuard guard;
    for (auto& callback : batch) {
        callback();
    }
}

std::chrono::milliseconds EpollLoop::getNextTimerTimeout() const {
    if (timers_.empty()) {
        return std::chrono::milliseconds(1000);
    }

    auto now = std::chrono::steady_clock::now();
    const auto& nextTimer = timers_.top();

    if (nextTimer.expiry <= now) {
        return std::chrono::milliseconds(0);
    }

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        nextTimer.expiry - now);

    return duration;
}

} // namespace pman::async
