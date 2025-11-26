#include "pman/process_health.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

namespace pman {

bool ProcessExistsCheck::check(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

TcpHealthCheck::TcpHealthCheck(std::string host, int port)
    : host_(std::move(host)), port_(port) {}

bool TcpHealthCheck::check(pid_t /*pid*/) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    int flags = ::fcntl(sock, F_GETFL, 0);
    ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        struct hostent* he = ::gethostbyname(host_.c_str());
        if (!he) {
            ::close(sock);
            return false;
        }
        std::memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    int rc = ::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        ::close(sock);
        return true;
    }

    if (errno != EINPROGRESS) {
        ::close(sock);
        return false;
    }

    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLOUT;

    auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout_);
    rc = ::poll(&pfd, 1, static_cast<int>(timeoutMs.count()));

    if (rc <= 0) {
        ::close(sock);
        return false;
    }

    int error = 0;
    socklen_t len = sizeof(error);
    if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        ::close(sock);
        return false;
    }

    ::close(sock);
    return true;
}

CommandHealthCheck::CommandHealthCheck(std::string command)
    : command_(std::move(command)) {}

bool CommandHealthCheck::check(pid_t /*pid*/) {
    pid_t child = ::fork();
    if (child < 0) {
        return false;
    }

    if (child == 0) {
        ::execl("/bin/sh", "sh", "-c", command_.c_str(), nullptr);
        ::_exit(127);
    }

    auto deadline = std::chrono::steady_clock::now() + timeout_;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result > 0) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    return false;
}

FileHealthCheck::FileHealthCheck(std::string path, std::chrono::nanoseconds maxAge)
    : path_(std::move(path)), maxAge_(maxAge) {}

bool FileHealthCheck::check(pid_t /*pid*/) {
    struct stat st{};
    if (::stat(path_.c_str(), &st) != 0) {
        return false;
    }

    auto mtime = std::chrono::system_clock::from_time_t(st.st_mtime);
    auto now = std::chrono::system_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::nanoseconds>(now - mtime);

    return age <= maxAge_;
}

HealthMonitor::HealthMonitor() {}

HealthMonitor::~HealthMonitor() {
    if (running_.load(std::memory_order_acquire)) {
        stop();
    }
}

void HealthMonitor::addProcess(const std::string& name, pid_t pid,
                               std::vector<std::unique_ptr<HealthCheck>> checks,
                               HealthCheckConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);

    MonitoredProcess proc;
    proc.pid = pid;
    proc.checks = std::move(checks);
    proc.config = config;
    proc.lastCheck = std::chrono::steady_clock::now() - config.interval;

    processes_[name] = std::move(proc);
}

void HealthMonitor::removeProcess(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    processes_.erase(name);
}

void HealthMonitor::updatePid(const std::string& name, pid_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = processes_.find(name);
    if (it != processes_.end()) {
        it->second.pid = pid;
        it->second.status = HealthStatus::Unknown;
        it->second.consecutiveFailures = 0;
        it->second.consecutiveSuccesses = 0;
    }
}

void HealthMonitor::start() {
    if (running_.load(std::memory_order_acquire)) {
        return;
    }

    running_.store(true, std::memory_order_release);

    monitorThread_ = std::make_unique<ManagedThread>(
        "pman-health",
        [this]() { monitorLoop(); });
}

void HealthMonitor::stop() {
    running_.store(false, std::memory_order_release);

    if (monitorThread_ && monitorThread_->joinable()) {
        monitorThread_->join();
    }
    monitorThread_.reset();
}

HealthStatus HealthMonitor::status(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = processes_.find(name);
    if (it == processes_.end()) {
        return HealthStatus::Unknown;
    }
    return it->second.status;
}

std::unordered_map<std::string, HealthStatus> HealthMonitor::allStatuses() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, HealthStatus> result;
    for (const auto& [name, proc] : processes_) {
        result[name] = proc.status;
    }
    return result;
}

void HealthMonitor::onHealthChange(
    std::function<void(const std::string&, HealthStatus, HealthStatus)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    healthChangeCallback_ = std::move(callback);
}

void HealthMonitor::monitorLoop() {
    while (running_.load(std::memory_order_acquire)) {
        auto now = std::chrono::steady_clock::now();

        std::vector<std::pair<std::string, MonitoredProcess*>> toCheck;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [name, proc] : processes_) {
                if (now - proc.lastCheck >= proc.config.interval) {
                    toCheck.emplace_back(name, &proc);
                }
            }
        }

        for (auto& [name, proc] : toCheck) {
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            auto it = processes_.find(name);
            if (it != processes_.end()) {
                checkProcess(name, it->second);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void HealthMonitor::checkProcess(const std::string& name, MonitoredProcess& proc) {
    bool allPassed = true;
    int passedCount = 0;
    int totalCount = static_cast<int>(proc.checks.size());

    for (auto& check : proc.checks) {
        if (!check->check(proc.pid)) {
            allPassed = false;
        } else {
            passedCount++;
        }
    }

    proc.lastCheck = std::chrono::steady_clock::now();

    HealthStatus oldStatus = proc.status;
    HealthStatus newStatus = oldStatus;

    if (allPassed) {
        proc.consecutiveSuccesses++;
        proc.consecutiveFailures = 0;

        if (proc.consecutiveSuccesses >= proc.config.healthyThreshold) {
            newStatus = HealthStatus::Healthy;
        }
    } else if (passedCount == 0) {
        proc.consecutiveFailures++;
        proc.consecutiveSuccesses = 0;

        if (proc.consecutiveFailures >= proc.config.unhealthyThreshold) {
            newStatus = HealthStatus::Unhealthy;
        }
    } else {
        proc.consecutiveSuccesses = 0;
        if (proc.consecutiveFailures > 0) {
            proc.consecutiveFailures++;
        }
        newStatus = HealthStatus::Degraded;
    }

    proc.status = newStatus;

    if (oldStatus != newStatus && healthChangeCallback_) {
        try {
            healthChangeCallback_(name, oldStatus, newStatus);
        } catch (...) {}
    }
}

}  // namespace pman
