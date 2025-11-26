#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pman/thread.hpp"
#include "pman/thread_attributes.hpp"

namespace pman {

class CancelledException : public std::exception {
public:
    const char* what() const noexcept override {
        return "Thread was cancelled";
    }
};

struct CancellationState {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<std::function<void()>> cleanupHandlers;
    std::atomic<size_t> handlerIdCounter{0};
};

class CleanupGuard {
public:
    CleanupGuard() = default;
    ~CleanupGuard();

    CleanupGuard(const CleanupGuard&) = delete;
    CleanupGuard& operator=(const CleanupGuard&) = delete;

    CleanupGuard(CleanupGuard&& other) noexcept;
    CleanupGuard& operator=(CleanupGuard&& other) noexcept;

    void dismiss() noexcept;

private:
    friend class ThreadCancellationToken;
    CleanupGuard(std::shared_ptr<CancellationState> state, size_t index);

    std::shared_ptr<CancellationState> state_;
    size_t index_{0};
    bool active_{false};
};

class ThreadCancellationToken {
public:
    [[nodiscard]] bool isCancelled() const noexcept;
    void throwIfCancelled() const;

    template <typename Callable>
    [[nodiscard]] CleanupGuard onCancel(Callable&& handler);

private:
    friend class CancellableThread;
    explicit ThreadCancellationToken(std::shared_ptr<CancellationState> state);

    std::shared_ptr<CancellationState> state_;
};

class CancellableThread {
public:
    CancellableThread() = default;
    CancellableThread(std::string name, std::function<void(ThreadCancellationToken&)> task,
                      ThreadAttributes attributes = {});
    ~CancellableThread();

    CancellableThread(const CancellableThread&) = delete;
    CancellableThread& operator=(const CancellableThread&) = delete;

    CancellableThread(CancellableThread&&) noexcept;
    CancellableThread& operator=(CancellableThread&&) noexcept;

    void cancel();

    [[nodiscard]] bool isCancelled() const noexcept;
    [[nodiscard]] bool joinable() const noexcept;

    void join();
    void detach();

    void setCpuAffinity(const CpuSet& set) const;
    void setScheduling(SchedulingPolicy policy, int priority);

    [[nodiscard]] pthread_t nativeHandle() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;

private:
    void runCleanupHandlers();

    ManagedThread thread_;
    std::shared_ptr<CancellationState> state_;
    std::string name_;
};

template <typename Callable>
CleanupGuard ThreadCancellationToken::onCancel(Callable&& handler) {
    if (!state_) {
        return CleanupGuard();
    }

    std::lock_guard<std::mutex> lock(state_->mutex);

    if (state_->cancelled.load(std::memory_order_acquire)) {
        handler();
        return CleanupGuard();
    }

    size_t index = state_->cleanupHandlers.size();
    state_->cleanupHandlers.push_back(std::forward<Callable>(handler));
    return CleanupGuard(state_, index);
}

}  // namespace pman
