#pragma once

#include <chrono>

#include <pthread.h>
#include <sched.h>

namespace pman {

void sleep_until_steady(std::chrono::steady_clock::time_point deadline);
void sleep_for_precise(std::chrono::nanoseconds duration);

void set_timer_slack(std::chrono::nanoseconds slack);

void set_thread_fifo(pthread_t handle, int priority);
void set_thread_round_robin(pthread_t handle, int priority);

struct DeadlineConfig {
    std::chrono::nanoseconds runtime{};
    std::chrono::nanoseconds deadline{};
    std::chrono::nanoseconds period{};
};

class DeadlineScope {
public:
    explicit DeadlineScope(const DeadlineConfig& config);
    ~DeadlineScope();

    DeadlineScope(const DeadlineScope&) = delete;
    DeadlineScope& operator=(const DeadlineScope&) = delete;

    DeadlineScope(DeadlineScope&&) = delete;
    DeadlineScope& operator=(DeadlineScope&&) = delete;

private:
    bool active_{false};
#ifdef __linux__
    bool prevCaptured_{false};
    int prevPolicy_{SCHED_OTHER};
    sched_param prevParam_{};
#endif
};

}  // namespace pman
