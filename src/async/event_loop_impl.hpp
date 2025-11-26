#pragma once

#include "pman/async/event_loop.hpp"
#include <chrono>
#include <functional>

namespace pman::async {

// Base class for event loop implementations
class EventLoopImpl {
public:
    virtual ~EventLoopImpl() = default;
    virtual void run() = 0;
    virtual bool runOnce(std::chrono::nanoseconds timeout) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const noexcept = 0;
    virtual BackendType backend() const noexcept = 0;
    virtual void addFd(int fd, Event events, EventCallback callback) = 0;
    virtual void modifyFd(int fd, Event events) = 0;
    virtual void removeFd(int fd) = 0;
    virtual uint64_t addTimer(std::chrono::nanoseconds duration, std::function<void()> callback) = 0;
    virtual uint64_t addPeriodicTimer(std::chrono::nanoseconds period, std::function<void()> callback) = 0;
    virtual bool cancelTimer(uint64_t timerId) = 0;
    virtual void post(std::function<void()> callback) = 0;
    virtual size_t activeFds() const noexcept = 0;
    virtual size_t activeTimers() const noexcept = 0;
};

} // namespace pman::async
