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

    // PHASE 2: Insert into multimap and store iterator for O(1) cancellation
    TimerEntry entry{id, std::move(callback), std::chrono::nanoseconds(0)};
    auto it = timersByExpiry_.emplace(expiry, std::move(entry));
    timerById_[id] = it;

    return id;
}

uint64_t EpollLoop::addPeriodicTimer(std::chrono::nanoseconds period, std::function<void()> callback) {
    uint64_t id = nextTimerId_++;
    auto expiry = std::chrono::steady_clock::now() + period;

    // PHASE 2: Insert into multimap with period info
    TimerEntry entry{id, std::move(callback), period};
    auto it = timersByExpiry_.emplace(expiry, std::move(entry));
    timerById_[id] = it;

    return id;
}

bool EpollLoop::cancelTimer(uint64_t timerId) {
    // PHASE 2: O(1) cancellation via iterator lookup
    auto it = timerById_.find(timerId);
    if (it == timerById_.end()) {
        return false;
    }

    timersByExpiry_.erase(it->second);
    timerById_.erase(it);
    return true;
}

bool EpollLoop::runOnce(std::chrono::nanoseconds timeout) {
    auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
    struct epoll_event events[64];

    int nfds = epoll_wait(epollFd_, events, 64, timeout_ms);
    if (nfds < 0 && errno != EINTR) {
        return false;
    }

    // PHASE 2: Only process timers if we have any and they're ready
    if (!timersByExpiry_.empty()) {
        auto now = std::chrono::steady_clock::now();
        if (timersByExpiry_.begin()->first <= now) {
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

// PHASE 3: Inline hot-path function with branch prediction hints
inline void EpollLoop::processPostedCallbacks() {
    if (postedCallbacks_.empty()) [[likely]] {
        return;
    }

    // Move callbacks to local vector to avoid issues if callbacks post more callbacks
    std::vector<std::function<void()>> callbacks;
    callbacks.swap(postedCallbacks_);

    for (auto& callback : callbacks) {
        if (callback) [[likely]] {
            detail::TrampolineGuard guard;
            callback();
        }
    }
}

size_t EpollLoop::activeFds() const noexcept {
    return fdCallbacks_.size();
}

size_t EpollLoop::activeTimers() const noexcept {
    return timerById_.size();
}

void EpollLoop::processTimers() {
    auto now = std::chrono::steady_clock::now();

    // PHASE 2: Batch callbacks for better throughput
    std::vector<std::function<void()>> batch;
    batch.reserve(32);

    // PHASE 2: Collect timers to reschedule (can't modify map while iterating)
    std::vector<std::pair<TimePoint, TimerEntry>> toReschedule;

    // PHASE 2: Iterate from beginning (earliest) until we hit unexpired timer
    auto it = timersByExpiry_.begin();
    while (it != timersByExpiry_.end()) {
        if (it->first > now) {
            break;  // All remaining timers are in the future
        }

        const auto& entry = it->second;
        uint64_t timerId = entry.id;

        // Copy callback before modifying containers
        auto callback = entry.callback;
        auto period = entry.period;

        // Remove from id lookup
        timerById_.erase(timerId);

        // If periodic, schedule for reschedule (can't insert while iterating)
        if (period.count() > 0) {
            TimerEntry newEntry{timerId, callback, period};
            toReschedule.emplace_back(now + period, std::move(newEntry));
        }

        // Batch the callback
        if (callback) {
            batch.push_back(std::move(callback));
        }

        // Erase and advance
        it = timersByExpiry_.erase(it);
    }

    // PHASE 2: Reschedule periodic timers
    for (auto& [expiry, entry] : toReschedule) {
        uint64_t id = entry.id;
        auto newIt = timersByExpiry_.emplace(expiry, std::move(entry));
        timerById_[id] = newIt;
    }

    // Execute all callbacks in batch (better cache locality)
    detail::TrampolineGuard guard;
    for (auto& callback : batch) {
        callback();
    }
}

// PHASE 3: Inline hot-path function with branch prediction hints
inline std::chrono::milliseconds EpollLoop::getNextTimerTimeout() const {
    if (timersByExpiry_.empty()) [[unlikely]] {
        return std::chrono::milliseconds(1000);
    }

    auto now = std::chrono::steady_clock::now();
    const auto& nextExpiry = timersByExpiry_.begin()->first;

    if (nextExpiry <= now) [[unlikely]] {
        return std::chrono::milliseconds(0);
    }

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        nextExpiry - now);

    return duration;
}

} // namespace pman::async
