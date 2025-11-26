#include "pman/async/event_loop.hpp"

#include "event_loop_impl.hpp"
#include "epoll_loop.hpp"
#include "io_uring_loop.hpp"
#include "io_uring_syscalls.hpp"

#include <stdexcept>

namespace pman::async {
namespace {

// Thread-local storage for current event loop
thread_local EventLoop* gCurrentLoop = nullptr;

std::unique_ptr<EventLoopImpl> createBackend(const EventLoopConfig& config) {
    BackendType backend = config.backend;

    // Auto-select best backend
    if (backend == BackendType::Auto) {
        // TODO: io_uring has timer callback corruption issues - default to epoll for now
        // Prefer io_uring if available and safe (kernel 5.12+)
        // if (detail::is_io_uring_safe()) {
        //     try {
        //         return std::make_unique<IoUringLoop>(config);
        //     } catch (...) {
        //         // Fall back to epoll if io_uring setup fails
        //         backend = BackendType::Epoll;
        //     }
        // } else {
            backend = BackendType::Epoll;
        // }
    }

    switch (backend) {
        case BackendType::Epoll:
            return std::make_unique<EpollLoop>(config);

        case BackendType::IoUring:
            return std::make_unique<IoUringLoop>(config);

        default:
            throw std::invalid_argument("Unknown backend type");
    }
}

} // namespace

EventLoop::EventLoop(EventLoopConfig config)
    : impl_(createBackend(config)) {}

EventLoop::~EventLoop() {
    if (gCurrentLoop == this) {
        gCurrentLoop = nullptr;
    }
}

EventLoop::EventLoop(EventLoop&& other) noexcept
    : impl_(std::move(other.impl_)) {}

EventLoop& EventLoop::operator=(EventLoop&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

EventLoop* EventLoop::current() noexcept {
    return gCurrentLoop;
}

void EventLoop::run() {
    if (gCurrentLoop != nullptr && gCurrentLoop != this) {
        throw std::logic_error("Another event loop is already running in this thread");
    }

    gCurrentLoop = this;
    impl_->run();
}

bool EventLoop::runOnce(std::chrono::nanoseconds timeout) {
    if (gCurrentLoop == nullptr) {
        gCurrentLoop = this;
    } else if (gCurrentLoop != this) {
        throw std::logic_error("Another event loop is already running in this thread");
    }

    return impl_->runOnce(timeout);
}

void EventLoop::stop() {
    impl_->stop();
}

bool EventLoop::isRunning() const noexcept {
    return impl_->isRunning();
}

BackendType EventLoop::backend() const noexcept {
    return impl_->backend();
}

void EventLoop::addFd(int fd, Event events, EventCallback callback) {
    impl_->addFd(fd, events, std::move(callback));
}

void EventLoop::modifyFd(int fd, Event events) {
    impl_->modifyFd(fd, events);
}

void EventLoop::removeFd(int fd) {
    impl_->removeFd(fd);
}

uint64_t EventLoop::addTimer(std::chrono::nanoseconds duration,
                              std::function<void()> callback) {
    return impl_->addTimer(duration, std::move(callback));
}

uint64_t EventLoop::addPeriodicTimer(std::chrono::nanoseconds period,
                                      std::function<void()> callback) {
    return impl_->addPeriodicTimer(period, std::move(callback));
}

bool EventLoop::cancelTimer(uint64_t timerId) {
    return impl_->cancelTimer(timerId);
}

void EventLoop::post(std::function<void()> callback) {
    impl_->post(std::move(callback));
}

size_t EventLoop::activeFds() const noexcept {
    return impl_->activeFds();
}

size_t EventLoop::activeTimers() const noexcept {
    return impl_->activeTimers();
}

} // namespace pman::async
