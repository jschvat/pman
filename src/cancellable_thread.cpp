#include "pman/cancellable_thread.hpp"

#include <utility>

namespace pman {

CleanupGuard::CleanupGuard(std::shared_ptr<CancellationState> state, size_t index)
    : state_(std::move(state)), index_(index), active_(true) {}

CleanupGuard::~CleanupGuard() {
    dismiss();
}

CleanupGuard::CleanupGuard(CleanupGuard&& other) noexcept
    : state_(std::move(other.state_)),
      index_(other.index_),
      active_(other.active_) {
    other.active_ = false;
}

CleanupGuard& CleanupGuard::operator=(CleanupGuard&& other) noexcept {
    if (this != &other) {
        dismiss();
        state_ = std::move(other.state_);
        index_ = other.index_;
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

void CleanupGuard::dismiss() noexcept {
    if (!active_ || !state_) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (index_ < state_->cleanupHandlers.size()) {
        state_->cleanupHandlers[index_] = nullptr;
    }
    active_ = false;
}

ThreadCancellationToken::ThreadCancellationToken(std::shared_ptr<CancellationState> state)
    : state_(std::move(state)) {}

bool ThreadCancellationToken::isCancelled() const noexcept {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
}

void ThreadCancellationToken::throwIfCancelled() const {
    if (isCancelled()) {
        throw CancelledException();
    }
}

CancellableThread::CancellableThread(std::string name,
                                     std::function<void(ThreadCancellationToken&)> task,
                                     ThreadAttributes attributes)
    : state_(std::make_shared<CancellationState>()), name_(std::move(name)) {

    auto stateCopy = state_;
    thread_ = ManagedThread(name_, [stateCopy, task = std::move(task)]() {
        ThreadCancellationToken token(stateCopy);
        try {
            task(token);
        } catch (const CancelledException&) {
        }
    }, std::move(attributes));
}

CancellableThread::~CancellableThread() {
    if (state_ && thread_.joinable()) {
        cancel();
    }
}

CancellableThread::CancellableThread(CancellableThread&& other) noexcept
    : thread_(std::move(other.thread_)),
      state_(std::move(other.state_)),
      name_(std::move(other.name_)) {}

CancellableThread& CancellableThread::operator=(CancellableThread&& other) noexcept {
    if (this != &other) {
        if (state_ && thread_.joinable()) {
            cancel();
            try {
                thread_.join();
            } catch (...) {}
        }

        thread_ = std::move(other.thread_);
        state_ = std::move(other.state_);
        name_ = std::move(other.name_);
    }
    return *this;
}

void CancellableThread::cancel() {
    if (!state_) {
        return;
    }

    state_->cancelled.store(true, std::memory_order_release);
    runCleanupHandlers();
}

void CancellableThread::runCleanupHandlers() {
    if (!state_) {
        return;
    }

    std::vector<std::function<void()>> handlers;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        handlers = std::move(state_->cleanupHandlers);
        state_->cleanupHandlers.clear();
    }

    for (auto it = handlers.rbegin(); it != handlers.rend(); ++it) {
        if (*it) {
            try {
                (*it)();
            } catch (...) {
            }
        }
    }
}

bool CancellableThread::isCancelled() const noexcept {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
}

bool CancellableThread::joinable() const noexcept {
    return thread_.joinable();
}

void CancellableThread::join() {
    thread_.join();
}

void CancellableThread::detach() {
    thread_.detach();
}

void CancellableThread::setCpuAffinity(const CpuSet& set) const {
    thread_.setCpuAffinity(set);
}

void CancellableThread::setScheduling(SchedulingPolicy policy, int priority) {
    thread_.setScheduling(policy, priority);
}

pthread_t CancellableThread::nativeHandle() const noexcept {
    return thread_.nativeHandle();
}

const std::string& CancellableThread::name() const noexcept {
    return name_;
}

}  // namespace pman
