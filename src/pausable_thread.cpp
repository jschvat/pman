#include "pman/pausable_thread.hpp"

#include <utility>

namespace pman {

PauseToken::PauseToken(std::shared_ptr<PauseControl> control)
    : control_(std::move(control)) {}

void PauseToken::checkPause() {
    if (!control_) {
        return;
    }

    if (control_->stopRequested.load(std::memory_order_acquire)) {
        throw StopRequestedException();
    }

    if (control_->pauseRequested.load(std::memory_order_acquire)) {
        FutexLockGuard lock(control_->mutex);

        control_->paused.store(true, std::memory_order_release);
        control_->pauseCv.notify_all();

        while (control_->pauseRequested.load(std::memory_order_acquire) &&
               !control_->stopRequested.load(std::memory_order_acquire)) {
            control_->resumeCv.wait(control_->mutex);
        }

        control_->paused.store(false, std::memory_order_release);

        if (control_->stopRequested.load(std::memory_order_acquire)) {
            throw StopRequestedException();
        }
    }
}

bool PauseToken::stopRequested() const noexcept {
    return control_ && control_->stopRequested.load(std::memory_order_acquire);
}

bool PauseToken::pauseRequested() const noexcept {
    return control_ && control_->pauseRequested.load(std::memory_order_acquire);
}

PausableThread::PausableThread(std::string name, std::function<void(PauseToken&)> task,
                               ThreadAttributes attributes)
    : control_(std::make_shared<PauseControl>()), name_(std::move(name)) {

    auto controlCopy = control_;
    thread_ = ManagedThread(name_, [controlCopy, task = std::move(task)]() {
        PauseToken token(controlCopy);
        try {
            task(token);
        } catch (const StopRequestedException&) {
        }
    }, std::move(attributes));
}

PausableThread::~PausableThread() {
    if (control_ && thread_.joinable()) {
        requestStop();
    }
}

PausableThread::PausableThread(PausableThread&& other) noexcept
    : thread_(std::move(other.thread_)),
      control_(std::move(other.control_)),
      name_(std::move(other.name_)) {}

PausableThread& PausableThread::operator=(PausableThread&& other) noexcept {
    if (this != &other) {
        if (control_ && thread_.joinable()) {
            requestStop();
            try {
                thread_.join();
            } catch (...) {}
        }

        thread_ = std::move(other.thread_);
        control_ = std::move(other.control_);
        name_ = std::move(other.name_);
    }
    return *this;
}

void PausableThread::pause() {
    if (!control_) {
        return;
    }

    FutexLockGuard lock(control_->mutex);
    control_->pauseRequested.store(true, std::memory_order_release);

    while (!control_->paused.load(std::memory_order_acquire) &&
           !control_->stopRequested.load(std::memory_order_acquire)) {
        if (!thread_.joinable()) {
            break;
        }
        control_->pauseCv.wait_for(control_->mutex, std::chrono::milliseconds(100));
    }
}

void PausableThread::resume() {
    if (!control_) {
        return;
    }

    FutexLockGuard lock(control_->mutex);
    control_->pauseRequested.store(false, std::memory_order_release);
    control_->resumeCv.notify_all();
}

void PausableThread::requestStop() {
    if (!control_) {
        return;
    }

    FutexLockGuard lock(control_->mutex);
    control_->stopRequested.store(true, std::memory_order_release);
    control_->pauseRequested.store(false, std::memory_order_release);
    control_->resumeCv.notify_all();
}

bool PausableThread::isPaused() const noexcept {
    return control_ && control_->paused.load(std::memory_order_acquire);
}

bool PausableThread::isStopRequested() const noexcept {
    return control_ && control_->stopRequested.load(std::memory_order_acquire);
}

bool PausableThread::joinable() const noexcept {
    return thread_.joinable();
}

void PausableThread::join() {
    thread_.join();
}

void PausableThread::detach() {
    thread_.detach();
}

void PausableThread::setCpuAffinity(const CpuSet& set) const {
    thread_.setCpuAffinity(set);
}

void PausableThread::setScheduling(SchedulingPolicy policy, int priority) {
    thread_.setScheduling(policy, priority);
}

pthread_t PausableThread::nativeHandle() const noexcept {
    return thread_.nativeHandle();
}

const std::string& PausableThread::name() const noexcept {
    return name_;
}

}  // namespace pman
