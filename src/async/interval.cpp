#include "pman/async/interval.hpp"
#include "pman/async/sleep.hpp"

namespace pman::async {

Interval Interval::create(Duration period, EventLoop* loop) {
    return Interval(period, loop);
}

Interval::Interval(Duration period, EventLoop* loop)
    : period_(period), loop_(loop) {}

Task<std::optional<uint64_t>> Interval::next() {
    if (stopped_) {
        co_return std::nullopt;
    }

    co_await sleep(period_, loop_);

    if (stopped_) {
        co_return std::nullopt;
    }

    co_return tick_++;
}

void Interval::stop() {
    stopped_ = true;
}

bool Interval::isStopped() const {
    return stopped_;
}

} // namespace pman::async
