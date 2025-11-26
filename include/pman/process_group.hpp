#pragma once

#include <chrono>
#include <optional>
#include <vector>

#include "pman/process.hpp"
#include "pman/process_builder.hpp"

namespace pman {

class ProcessGroup {
public:
    ProcessGroup() = default;
    ~ProcessGroup();

    ProcessGroup(const ProcessGroup&) = delete;
    ProcessGroup& operator=(const ProcessGroup&) = delete;

    ProcessGroup(ProcessGroup&&) noexcept;
    ProcessGroup& operator=(ProcessGroup&&) noexcept;

    ProcessHandle& add(ProcessConfig config);
    ProcessHandle& add(ProcessBuilder&& builder);

    void sendSignalAll(int signal);
    void terminateAll();
    void killAll();
    void stopAll();
    void continueAll();

    int waitAny();
    std::vector<int> waitAll();
    bool waitAllFor(std::chrono::nanoseconds timeout);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] ProcessHandle& at(std::size_t index);
    [[nodiscard]] const ProcessHandle& at(std::size_t index) const;

    [[nodiscard]] ProcessHandle& operator[](std::size_t index);
    [[nodiscard]] const ProcessHandle& operator[](std::size_t index) const;

    auto begin() { return processes_.begin(); }
    auto end() { return processes_.end(); }
    auto begin() const { return processes_.begin(); }
    auto end() const { return processes_.end(); }

    [[nodiscard]] std::optional<pid_t> pgid() const noexcept { return pgid_; }

    void setProcessGroup(pid_t pgid);
    void createProcessGroup();

private:
    std::vector<ProcessHandle> processes_;
    std::optional<pid_t> pgid_;
};

}  // namespace pman
