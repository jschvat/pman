#include "pman/scheduling.hpp"

#include <errno.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include <system_error>
#include <stdexcept>

#ifdef __linux__
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif
#include <linux/sched.h>
#include <sys/syscall.h>
#endif

namespace pman {
namespace {

timespec toTimespec(std::chrono::nanoseconds ns) {
    timespec ts;
    ts.tv_sec = static_cast<time_t>(ns.count() / 1'000'000'000LL);
    ts.tv_nsec = static_cast<long>(ns.count() % 1'000'000'000LL);
    if (ts.tv_nsec < 0) {
        ts.tv_sec -= 1;
        ts.tv_nsec += 1'000'000'000L;
    }
    return ts;
}

void nanosleep_until(timespec ts) {
    while (true) {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        if (rc == 0) {
            break;
        }
        if (rc != EINTR) {
            throw std::system_error(rc, std::generic_category(), "clock_nanosleep");
        }
    }
}

#ifdef __linux__
struct sched_attr_local {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};

int sched_setattr_local(pid_t pid, const sched_attr_local* attr, unsigned int flags) {
    return syscall(SYS_sched_setattr, pid, attr, flags);
}
#endif

}  // namespace

void sleep_until_steady(std::chrono::steady_clock::time_point deadline) {
    auto now = std::chrono::steady_clock::now();
    if (deadline <= now) {
        return;
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch());
    nanosleep_until(toTimespec(ns));
}

void sleep_for_precise(std::chrono::nanoseconds duration) {
    sleep_until_steady(std::chrono::steady_clock::now() + duration);
}

void set_timer_slack(std::chrono::nanoseconds slack) {
#ifdef PR_SET_TIMERSLACK
    unsigned long ns = static_cast<unsigned long>(slack.count() < 0 ? 0 : slack.count());
    if (prctl(PR_SET_TIMERSLACK, ns, 0, 0, 0) != 0) {
        throw std::system_error(errno, std::generic_category(), "prctl(PR_SET_TIMERSLACK)");
    }
#else
    (void)slack;
#endif
}

void set_thread_fifo(pthread_t handle, int priority) {
    sched_param param{};
    param.sched_priority = priority;
    int rc = pthread_setschedparam(handle, SCHED_FIFO, &param);
    if (rc != 0) {
        throw std::system_error(rc, std::generic_category(), "pthread_setschedparam");
    }
}

void set_thread_round_robin(pthread_t handle, int priority) {
    sched_param param{};
    param.sched_priority = priority;
    int rc = pthread_setschedparam(handle, SCHED_RR, &param);
    if (rc != 0) {
        throw std::system_error(rc, std::generic_category(), "pthread_setschedparam");
    }
}

DeadlineScope::DeadlineScope(const DeadlineConfig& config) {
#ifdef __linux__
    if (config.runtime.count() <= 0 || config.deadline.count() <= 0 || config.period.count() <= 0) {
        throw std::invalid_argument("runtime/deadline/period must be positive");
    }
    if (config.runtime > config.deadline || config.deadline > config.period) {
        throw std::invalid_argument("runtime <= deadline <= period");
    }

    pthread_getschedparam(pthread_self(), &prevPolicy_, &prevParam_);
    prevCaptured_ = true;

    sched_attr_local attr{};
    attr.size = sizeof(attr);
    attr.sched_policy = SCHED_DEADLINE;
    attr.sched_runtime = static_cast<uint64_t>(config.runtime.count());
    attr.sched_deadline = static_cast<uint64_t>(config.deadline.count());
    attr.sched_period = static_cast<uint64_t>(config.period.count());

    pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    if (sched_setattr_local(tid, &attr, 0) != 0) {
        prevCaptured_ = false;
        throw std::system_error(errno, std::generic_category(), "sched_setattr (SCHED_DEADLINE)");
    }
    active_ = true;
#else
    (void)config;
#endif
}

DeadlineScope::~DeadlineScope() {
#ifdef __linux__
    if (active_) {
        if (prevCaptured_) {
            pthread_setschedparam(pthread_self(), prevPolicy_, &prevParam_);
        } else {
            sched_attr_local attr{};
            attr.size = sizeof(attr);
            attr.sched_policy = SCHED_OTHER;
            pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
            sched_setattr_local(tid, &attr, 0);
        }
    }
#endif
}

}  // namespace pman
