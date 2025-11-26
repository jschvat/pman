#include "pman/timer_wheel.hpp"
#include "pman/thread.hpp"

#include <errno.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <stdexcept>
#include <system_error>

namespace pman {

TimerWheel::TimerWheel(Config config) : config_(config) {
    if (config_.wheelSize == 0 || config_.numWheels == 0) {
        throw std::invalid_argument("Invalid timer wheel configuration");
    }

    wheels_.resize(config_.numWheels);
    currentSlots_.resize(config_.numWheels, 0);

    for (std::size_t i = 0; i < config_.numWheels; ++i) {
        wheels_[i].resize(config_.wheelSize);
    }

    lastTick_ = std::chrono::steady_clock::now();

    timerFd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerFd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "timerfd_create");
    }
}

TimerWheel::~TimerWheel() {
    stop();
    if (timerFd_ >= 0) {
        ::close(timerFd_);
    }
}

std::uint64_t TimerWheel::schedule(std::chrono::nanoseconds delay, TimerCallback callback) {
    if (!callback) {
        throw std::invalid_argument("Timer callback cannot be null");
    }

    TimerEntry entry;
    entry.id = nextId_.fetch_add(1, std::memory_order_relaxed);
    entry.expiry = std::chrono::steady_clock::now() + delay;
    entry.period = std::chrono::nanoseconds::zero();
    entry.callback = std::move(callback);
    entry.cancelled = std::make_shared<std::atomic<bool>>(false);

    std::lock_guard<std::mutex> lock(mutex_);
    insert(std::move(entry));
    activeCount_.fetch_add(1, std::memory_order_relaxed);

    if (running_.load(std::memory_order_acquire)) {
        armTimerFd();
    }

    return entry.id;
}

std::uint64_t TimerWheel::schedulePeriodic(std::chrono::nanoseconds period, TimerCallback callback) {
    if (!callback) {
        throw std::invalid_argument("Timer callback cannot be null");
    }
    if (period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("Period must be positive");
    }

    TimerEntry entry;
    entry.id = nextId_.fetch_add(1, std::memory_order_relaxed);
    entry.expiry = std::chrono::steady_clock::now() + period;
    entry.period = period;
    entry.callback = std::move(callback);
    entry.cancelled = std::make_shared<std::atomic<bool>>(false);

    std::lock_guard<std::mutex> lock(mutex_);
    insert(std::move(entry));
    activeCount_.fetch_add(1, std::memory_order_relaxed);

    if (running_.load(std::memory_order_acquire)) {
        armTimerFd();
    }

    return entry.id;
}

bool TimerWheel::cancel(std::uint64_t timerId) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& wheel : wheels_) {
        for (auto& slot : wheel) {
            for (auto& entry : slot) {
                if (entry.id == timerId && !entry.cancelled->load(std::memory_order_acquire)) {
                    entry.cancelled->store(true, std::memory_order_release);
                    activeCount_.fetch_sub(1, std::memory_order_relaxed);
                    return true;
                }
            }
        }
    }
    return false;
}

void TimerWheel::tick() {
    advance(config_.tickDuration);
}

void TimerWheel::advance(std::chrono::nanoseconds elapsed) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto ticksToAdvance = elapsed / config_.tickDuration;
    for (std::int64_t i = 0; i < ticksToAdvance; ++i) {
        currentSlots_[0] = (currentSlots_[0] + 1) % config_.wheelSize;

        if (currentSlots_[0] == 0) {
            for (std::size_t w = 1; w < config_.numWheels; ++w) {
                cascade(w);
                currentSlots_[w] = (currentSlots_[w] + 1) % config_.wheelSize;
                if (currentSlots_[w] != 0) {
                    break;
                }
            }
        }
    }

    lastTick_ = std::chrono::steady_clock::now();
}

std::size_t TimerWheel::processExpired() {
    std::vector<TimerEntry> expired;
    auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto& slot = wheels_[0][currentSlots_[0]];
        auto it = slot.begin();
        while (it != slot.end()) {
            if (it->cancelled->load(std::memory_order_acquire)) {
                it = slot.erase(it);
                continue;
            }
            if (it->expiry <= now) {
                expired.push_back(std::move(*it));
                it = slot.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (auto& entry : expired) {
        if (!entry.cancelled->load(std::memory_order_acquire)) {
            try {
                entry.callback();
            } catch (...) {
            }

            if (entry.period > std::chrono::nanoseconds::zero() &&
                !entry.cancelled->load(std::memory_order_acquire)) {
                entry.expiry = std::chrono::steady_clock::now() + entry.period;
                std::lock_guard<std::mutex> lock(mutex_);
                insert(std::move(entry));
            } else {
                activeCount_.fetch_sub(1, std::memory_order_relaxed);
            }
        } else {
            activeCount_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    return expired.size();
}

std::chrono::nanoseconds TimerWheel::timeUntilNextExpiry() const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto earliest = now + std::chrono::hours(24);

    for (const auto& wheel : wheels_) {
        for (const auto& slot : wheel) {
            for (const auto& entry : slot) {
                if (!entry.cancelled->load(std::memory_order_acquire)) {
                    if (entry.expiry < earliest) {
                        earliest = entry.expiry;
                    }
                }
            }
        }
    }

    if (earliest <= now) {
        return std::chrono::nanoseconds::zero();
    }
    return earliest - now;
}

std::size_t TimerWheel::activeTimers() const noexcept {
    return activeCount_.load(std::memory_order_relaxed);
}

std::size_t TimerWheel::pendingExpirations() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    auto now = std::chrono::steady_clock::now();

    for (const auto& wheel : wheels_) {
        for (const auto& slot : wheel) {
            for (const auto& entry : slot) {
                if (!entry.cancelled->load(std::memory_order_acquire) && entry.expiry <= now) {
                    ++count;
                }
            }
        }
    }
    return count;
}

void TimerWheel::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(true, std::memory_order_release);
    armTimerFd();

    timerThread_ = std::make_unique<ManagedThread>("pman-timers", [this]() {
        while (running_.load(std::memory_order_acquire)) {
            pollfd pfd{};
            pfd.fd = timerFd_;
            pfd.events = POLLIN;

            int rc = ::poll(&pfd, 1, 100);
            if (rc > 0 && (pfd.revents & POLLIN)) {
                std::uint64_t expirations;
                ::read(timerFd_, &expirations, sizeof(expirations));

                tick();
                processExpired();
                armTimerFd();
            } else if (rc == 0) {
                tick();
                processExpired();
                armTimerFd();
            }
        }
    });
}

void TimerWheel::stop() {
    running_.store(false, std::memory_order_release);

    if (timerThread_ && timerThread_->joinable()) {
        timerThread_->join();
    }
    timerThread_.reset();
}

void TimerWheel::insert(TimerEntry entry) {
    auto now = std::chrono::steady_clock::now();
    auto delay = entry.expiry > now ? entry.expiry - now : std::chrono::nanoseconds::zero();

    for (std::size_t w = 0; w < config_.numWheels; ++w) {
        std::size_t slot = computeSlot(delay, w);
        if (slot < config_.wheelSize || w == config_.numWheels - 1) {
            slot = std::min(slot, config_.wheelSize - 1);
            std::size_t actualSlot = (currentSlots_[w] + slot) % config_.wheelSize;
            wheels_[w][actualSlot].push_back(std::move(entry));
            return;
        }
    }
}

void TimerWheel::cascade(std::size_t wheelIndex) {
    if (wheelIndex >= config_.numWheels) {
        return;
    }

    auto& slot = wheels_[wheelIndex][currentSlots_[wheelIndex]];
    std::deque<TimerEntry> entries = std::move(slot);
    slot.clear();

    for (auto& entry : entries) {
        if (!entry.cancelled->load(std::memory_order_acquire)) {
            insert(std::move(entry));
        }
    }
}

std::size_t TimerWheel::computeSlot(std::chrono::nanoseconds delay, std::size_t wheelIndex) const {
    auto ticksPerSlot = config_.tickDuration;
    for (std::size_t i = 0; i < wheelIndex; ++i) {
        ticksPerSlot *= config_.wheelSize;
    }

    auto ticks = delay / ticksPerSlot;
    return static_cast<std::size_t>(ticks);
}

void TimerWheel::armTimerFd() {
    auto nextExpiry = timeUntilNextExpiry();

    if (nextExpiry > std::chrono::hours(1)) {
        nextExpiry = config_.tickDuration;
    }

    if (nextExpiry < config_.tickDuration) {
        nextExpiry = config_.tickDuration;
    }

    struct itimerspec ts{};
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(nextExpiry);
    auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(nextExpiry - secs);

    ts.it_value.tv_sec = secs.count();
    ts.it_value.tv_nsec = nsecs.count();

    if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
        ts.it_value.tv_nsec = 1;
    }

    timerfd_settime(timerFd_, 0, &ts, nullptr);
}

}  // namespace pman
