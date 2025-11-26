#include "pman/cron_scheduler.hpp"

#include <errno.h>
#include <poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace pman {

CronExpression::CronExpression() {
    setAll();
}

void CronExpression::setAll() {
    minutes_.set();
    hours_.set();
    daysOfMonth_.set();
    daysOfMonth_.reset(0);
    months_.set();
    months_.reset(0);
    daysOfWeek_.set();
}

CronExpression CronExpression::parse(std::string_view expression) {
    CronExpression cron;

    cron.minutes_.reset();
    cron.hours_.reset();
    cron.daysOfMonth_.reset();
    cron.months_.reset();
    cron.daysOfWeek_.reset();

    std::string exprStr{expression};
    std::istringstream iss{exprStr};
    std::string minStr, hourStr, domStr, monthStr, dowStr;

    if (!(iss >> minStr >> hourStr >> domStr >> monthStr >> dowStr)) {
        throw std::invalid_argument("Invalid cron expression format");
    }

    auto parseField = [](const std::string& field, std::size_t maxVal, auto& bitset) {
        if (field == "*") {
            for (std::size_t i = 0; i <= maxVal; ++i) {
                bitset.set(i);
            }
            return;
        }

        std::size_t pos = 0;
        while (pos < field.size()) {
            std::size_t end = field.find(',', pos);
            if (end == std::string::npos) {
                end = field.size();
            }

            std::string part = field.substr(pos, end - pos);

            std::size_t rangePos = part.find('-');
            if (rangePos != std::string::npos) {
                int start, finish;
                std::from_chars(part.data(), part.data() + rangePos, start);
                std::from_chars(part.data() + rangePos + 1, part.data() + part.size(), finish);
                for (int i = start; i <= finish && static_cast<std::size_t>(i) <= maxVal; ++i) {
                    bitset.set(static_cast<std::size_t>(i));
                }
            } else {
                int val;
                std::from_chars(part.data(), part.data() + part.size(), val);
                if (static_cast<std::size_t>(val) <= maxVal) {
                    bitset.set(static_cast<std::size_t>(val));
                }
            }

            pos = end + 1;
        }
    };

    parseField(minStr, 59, cron.minutes_);
    parseField(hourStr, 23, cron.hours_);
    parseField(domStr, 31, cron.daysOfMonth_);
    parseField(monthStr, 12, cron.months_);
    parseField(dowStr, 6, cron.daysOfWeek_);

    return cron;
}

CronExpression CronExpression::everyMinute() {
    CronExpression cron;
    return cron;
}

CronExpression CronExpression::everyHour() {
    CronExpression cron;
    cron.minutes_.reset();
    cron.minutes_.set(0);
    return cron;
}

CronExpression CronExpression::daily(int hour, int minute) {
    CronExpression cron;
    cron.minutes_.reset();
    cron.minutes_.set(static_cast<std::size_t>(minute));
    cron.hours_.reset();
    cron.hours_.set(static_cast<std::size_t>(hour));
    return cron;
}

CronExpression CronExpression::weekly(int dayOfWeek, int hour, int minute) {
    CronExpression cron;
    cron.minutes_.reset();
    cron.minutes_.set(static_cast<std::size_t>(minute));
    cron.hours_.reset();
    cron.hours_.set(static_cast<std::size_t>(hour));
    cron.daysOfWeek_.reset();
    cron.daysOfWeek_.set(static_cast<std::size_t>(dayOfWeek));
    return cron;
}

CronExpression CronExpression::monthly(int dayOfMonth, int hour, int minute) {
    CronExpression cron;
    cron.minutes_.reset();
    cron.minutes_.set(static_cast<std::size_t>(minute));
    cron.hours_.reset();
    cron.hours_.set(static_cast<std::size_t>(hour));
    cron.daysOfMonth_.reset();
    cron.daysOfMonth_.set(static_cast<std::size_t>(dayOfMonth));
    return cron;
}

CronExpression& CronExpression::minute(int value) {
    if (value >= 0 && value < 60) {
        minutes_.reset();
        minutes_.set(static_cast<std::size_t>(value));
    }
    return *this;
}

CronExpression& CronExpression::hour(int value) {
    if (value >= 0 && value < 24) {
        hours_.reset();
        hours_.set(static_cast<std::size_t>(value));
    }
    return *this;
}

CronExpression& CronExpression::dayOfMonth(int value) {
    if (value >= 1 && value <= 31) {
        daysOfMonth_.reset();
        daysOfMonth_.set(static_cast<std::size_t>(value));
    }
    return *this;
}

CronExpression& CronExpression::month(int value) {
    if (value >= 1 && value <= 12) {
        months_.reset();
        months_.set(static_cast<std::size_t>(value));
    }
    return *this;
}

CronExpression& CronExpression::dayOfWeek(int value) {
    if (value >= 0 && value <= 6) {
        daysOfWeek_.reset();
        daysOfWeek_.set(static_cast<std::size_t>(value));
    }
    return *this;
}

CronExpression& CronExpression::minuteRange(int start, int end) {
    minutes_.reset();
    for (int i = start; i <= end && i < 60; ++i) {
        minutes_.set(static_cast<std::size_t>(i));
    }
    return *this;
}

CronExpression& CronExpression::hourRange(int start, int end) {
    hours_.reset();
    for (int i = start; i <= end && i < 24; ++i) {
        hours_.set(static_cast<std::size_t>(i));
    }
    return *this;
}

std::chrono::system_clock::time_point
CronExpression::nextAfter(std::chrono::system_clock::time_point after) const {
    std::time_t t = std::chrono::system_clock::to_time_t(after);
    std::tm tm;
    localtime_r(&t, &tm);

    tm.tm_sec = 0;
    tm.tm_min++;

    for (int iterations = 0; iterations < 366 * 24 * 60; ++iterations) {
        if (tm.tm_min >= 60) {
            tm.tm_min = 0;
            tm.tm_hour++;
        }
        if (tm.tm_hour >= 24) {
            tm.tm_hour = 0;
            tm.tm_mday++;
        }

        std::time_t check = std::mktime(&tm);
        localtime_r(&check, &tm);

        if (!months_.test(static_cast<std::size_t>(tm.tm_mon + 1))) {
            tm.tm_mday = 1;
            tm.tm_hour = 0;
            tm.tm_min = 0;
            tm.tm_mon++;
            if (tm.tm_mon >= 12) {
                tm.tm_mon = 0;
                tm.tm_year++;
            }
            continue;
        }

        if (!daysOfMonth_.test(static_cast<std::size_t>(tm.tm_mday)) ||
            !daysOfWeek_.test(static_cast<std::size_t>(tm.tm_wday))) {
            tm.tm_hour = 0;
            tm.tm_min = 0;
            tm.tm_mday++;
            continue;
        }

        if (!hours_.test(static_cast<std::size_t>(tm.tm_hour))) {
            tm.tm_min = 0;
            tm.tm_hour++;
            continue;
        }

        if (!minutes_.test(static_cast<std::size_t>(tm.tm_min))) {
            tm.tm_min++;
            continue;
        }

        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    }

    throw std::runtime_error("Could not find next cron execution time");
}

std::string CronExpression::toString() const {
    std::ostringstream oss;

    auto fieldToString = [](const auto& bitset, std::size_t maxVal) {
        std::vector<int> values;
        for (std::size_t i = 0; i <= maxVal; ++i) {
            if (bitset.test(i)) {
                values.push_back(static_cast<int>(i));
            }
        }

        if (values.size() == maxVal + 1) {
            return std::string("*");
        }

        std::ostringstream s;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) s << ",";
            s << values[i];
        }
        return s.str();
    };

    oss << fieldToString(minutes_, 59) << " "
        << fieldToString(hours_, 23) << " "
        << fieldToString(daysOfMonth_, 31) << " "
        << fieldToString(months_, 12) << " "
        << fieldToString(daysOfWeek_, 6);

    return oss.str();
}

CronScheduler::CronScheduler() {
    timerFd_ = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerFd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "timerfd_create");
    }
}

CronScheduler::~CronScheduler() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
    if (timerFd_ >= 0) {
        ::close(timerFd_);
    }
}

std::uint64_t CronScheduler::schedule(const std::string& name, CronExpression expr,
                                       std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("Task cannot be null");
    }

    std::uint64_t taskId = nextTaskId_.fetch_add(1, std::memory_order_relaxed);

    CronTask cronTask;
    cronTask.id = taskId;
    cronTask.name = name;
    cronTask.expression = std::move(expr);
    cronTask.task = std::move(task);
    cronTask.nextRun = cronTask.expression.nextAfter(std::chrono::system_clock::now());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_[taskId] = std::move(cronTask);
    }

    if (running_.load(std::memory_order_acquire)) {
        scheduleWakeup();
    }

    return taskId;
}

bool CronScheduler::cancel(std::uint64_t taskId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) {
        return false;
    }

    it->second.active = false;
    return true;
}

void CronScheduler::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(true, std::memory_order_release);
    scheduleWakeup();

    schedulerThread_ = std::make_unique<ManagedThread>("pman-cron", [this]() {
        schedulerLoop();
    });
}

void CronScheduler::stop() {
    running_.store(false, std::memory_order_release);

    if (schedulerThread_ && schedulerThread_->joinable()) {
        schedulerThread_->join();
    }
    schedulerThread_.reset();
}

std::chrono::system_clock::time_point CronScheduler::nextExecution(std::uint64_t taskId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(taskId);
    if (it == tasks_.end()) {
        throw std::runtime_error("Unknown task ID");
    }

    return it->second.nextRun;
}

std::vector<CronScheduler::TaskInfo> CronScheduler::listTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<TaskInfo> result;
    result.reserve(tasks_.size());

    for (const auto& [id, task] : tasks_) {
        if (task.active) {
            result.push_back({id, task.name, task.nextRun, task.executionCount});
        }
    }

    return result;
}

void CronScheduler::schedulerLoop() {
    while (running_.load(std::memory_order_acquire)) {
        pollfd pfd{};
        pfd.fd = timerFd_;
        pfd.events = POLLIN;

        int rc = ::poll(&pfd, 1, 1000);
        if (rc > 0 && (pfd.revents & POLLIN)) {
            std::uint64_t expirations;
            ::read(timerFd_, &expirations, sizeof(expirations));
        }

        auto now = std::chrono::system_clock::now();
        std::vector<std::function<void()>> toExecute;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [id, task] : tasks_) {
                if (!task.active) {
                    continue;
                }

                if (task.nextRun <= now) {
                    toExecute.push_back(task.task);
                    task.executionCount++;
                    task.nextRun = task.expression.nextAfter(now);
                }
            }
        }

        for (auto& task : toExecute) {
            try {
                task();
            } catch (...) {
            }
        }

        if (running_.load(std::memory_order_acquire)) {
            scheduleWakeup();
        }
    }
}

void CronScheduler::scheduleWakeup() {
    std::chrono::system_clock::time_point earliest;
    bool found = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, task] : tasks_) {
            if (task.active) {
                if (!found || task.nextRun < earliest) {
                    earliest = task.nextRun;
                    found = true;
                }
            }
        }
    }

    if (!found) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto delay = earliest > now ? earliest - now : std::chrono::seconds(1);

    if (delay > std::chrono::hours(1)) {
        delay = std::chrono::hours(1);
    }

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(delay);
    auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(delay - secs);

    struct itimerspec ts{};
    ts.it_value.tv_sec = secs.count();
    ts.it_value.tv_nsec = nsecs.count();

    if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
        ts.it_value.tv_nsec = 1;
    }

    timerfd_settime(timerFd_, 0, &ts, nullptr);
}

}  // namespace pman
