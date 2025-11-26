#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pman/thread.hpp"

namespace pman {

enum class HealthStatus {
    Unknown,
    Healthy,
    Unhealthy,
    Degraded,
};

struct HealthCheckConfig {
    std::chrono::nanoseconds interval{std::chrono::seconds(30)};
    std::chrono::nanoseconds timeout{std::chrono::seconds(5)};
    int unhealthyThreshold{3};
    int healthyThreshold{2};
};

class HealthCheck {
public:
    virtual ~HealthCheck() = default;
    virtual bool check(pid_t pid) = 0;
    [[nodiscard]] virtual std::string name() const = 0;
};

class ProcessExistsCheck : public HealthCheck {
public:
    bool check(pid_t pid) override;
    [[nodiscard]] std::string name() const override { return "process_exists"; }
};

class TcpHealthCheck : public HealthCheck {
public:
    explicit TcpHealthCheck(std::string host, int port);
    bool check(pid_t pid) override;
    [[nodiscard]] std::string name() const override { return "tcp:" + host_ + ":" + std::to_string(port_); }

private:
    std::string host_;
    int port_;
    std::chrono::nanoseconds timeout_{std::chrono::seconds(5)};
};

class CommandHealthCheck : public HealthCheck {
public:
    explicit CommandHealthCheck(std::string command);
    bool check(pid_t pid) override;
    [[nodiscard]] std::string name() const override { return "command"; }

private:
    std::string command_;
    std::chrono::nanoseconds timeout_{std::chrono::seconds(10)};
};

class FileHealthCheck : public HealthCheck {
public:
    explicit FileHealthCheck(std::string path, std::chrono::nanoseconds maxAge = std::chrono::minutes(5));
    bool check(pid_t pid) override;
    [[nodiscard]] std::string name() const override { return "file:" + path_; }

private:
    std::string path_;
    std::chrono::nanoseconds maxAge_;
};

class HealthMonitor {
public:
    HealthMonitor();
    ~HealthMonitor();

    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;

    void addProcess(const std::string& name, pid_t pid,
                    std::vector<std::unique_ptr<HealthCheck>> checks,
                    HealthCheckConfig config = {});
    void removeProcess(const std::string& name);
    void updatePid(const std::string& name, pid_t pid);

    void start();
    void stop();

    [[nodiscard]] HealthStatus status(const std::string& name) const;
    [[nodiscard]] std::unordered_map<std::string, HealthStatus> allStatuses() const;

    void onHealthChange(std::function<void(const std::string&, HealthStatus, HealthStatus)> callback);

private:
    struct MonitoredProcess {
        pid_t pid;
        std::vector<std::unique_ptr<HealthCheck>> checks;
        HealthCheckConfig config;
        HealthStatus status{HealthStatus::Unknown};
        int consecutiveFailures{0};
        int consecutiveSuccesses{0};
        std::chrono::steady_clock::time_point lastCheck;
    };

    void monitorLoop();
    void checkProcess(const std::string& name, MonitoredProcess& proc);

    std::unordered_map<std::string, MonitoredProcess> processes_;
    mutable std::mutex mutex_;
    std::unique_ptr<ManagedThread> monitorThread_;
    std::atomic<bool> running_{false};

    std::function<void(const std::string&, HealthStatus, HealthStatus)> healthChangeCallback_;
};

}  // namespace pman
