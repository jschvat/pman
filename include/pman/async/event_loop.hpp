#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace pman::async {

enum class BackendType {
    Auto,
    Epoll,
    IoUring,
};

enum class Event : uint32_t {
    Read = 1 << 0,
    Write = 1 << 1,
    Error = 1 << 2,
    HangUp = 1 << 3,
};

inline Event operator|(Event a, Event b) {
    return static_cast<Event>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline Event operator&(Event a, Event b) {
    return static_cast<Event>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

using EventCallback = std::function<void(Event)>;

struct EventLoopConfig {
    BackendType backend = BackendType::Auto;
    size_t io_uring_entries = 256;
};

class EventLoopImpl;

class EventLoop {
public:
    explicit EventLoop(EventLoopConfig config = EventLoopConfig{});
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    EventLoop(EventLoop&& other) noexcept;
    EventLoop& operator=(EventLoop&& other) noexcept;

    static EventLoop* current() noexcept;

    void run();
    bool runOnce(std::chrono::nanoseconds timeout = std::chrono::milliseconds(0));
    void stop();
    bool isRunning() const noexcept;

    BackendType backend() const noexcept;

    void addFd(int fd, Event events, EventCallback callback);
    void modifyFd(int fd, Event events);
    void removeFd(int fd);

    uint64_t addTimer(std::chrono::nanoseconds duration, std::function<void()> callback);
    uint64_t addPeriodicTimer(std::chrono::nanoseconds period, std::function<void()> callback);
    bool cancelTimer(uint64_t timerId);

    void post(std::function<void()> callback);

    size_t activeFds() const noexcept;
    size_t activeTimers() const noexcept;

private:
    std::unique_ptr<EventLoopImpl> impl_;
};

} // namespace pman::async
