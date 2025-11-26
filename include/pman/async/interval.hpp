#pragma once
#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/time.hpp"
#include <optional>

namespace pman::async {
class Interval {
public:
    static Interval create(Duration period, EventLoop* loop = nullptr);
    Task<std::optional<uint64_t>> next();
    void stop();
    bool isStopped() const;
private:
    Interval(Duration period, EventLoop* loop);
    Duration period_;
    EventLoop* loop_;
    uint64_t tick_{0};
    bool stopped_{false};
};
} // namespace pman::async
