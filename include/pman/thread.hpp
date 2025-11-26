#pragma once

#include <pthread.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "pman/thread_attributes.hpp"

namespace pman {

namespace detail {
struct ThreadState {
    std::function<void()> task;
    std::exception_ptr exception;
    std::string name;
};
}  // namespace detail

class ManagedThread {
public:
    ManagedThread() = default;
    ManagedThread(std::string name, std::function<void()> task, ThreadAttributes attributes = {});
    ~ManagedThread();

    ManagedThread(const ManagedThread&) = delete;
    ManagedThread& operator=(const ManagedThread&) = delete;

    ManagedThread(ManagedThread&& other) noexcept;
    ManagedThread& operator=(ManagedThread&& other) noexcept;

    [[nodiscard]] bool joinable() const noexcept { return joinable_; }
    void join();
    void detach();

    void setCpuAffinity(const CpuSet& set) const;
    void setScheduling(SchedulingPolicy policy, int priority);
    void setName(std::string_view name);

    [[nodiscard]] pthread_t nativeHandle() const noexcept { return thread_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

private:
    void start(std::function<void()> task);
    static void* entryPoint(void* data) noexcept;
    void cleanupState();

    pthread_t thread_{};
    bool joinable_{false};
    bool started_{false};
    std::string name_;
    ThreadAttributes attributes_{};
    std::shared_ptr<detail::ThreadState> state_;
};

}  // namespace pman
