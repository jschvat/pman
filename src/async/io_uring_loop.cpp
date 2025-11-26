#include "io_uring_loop.hpp"
#include "io_uring_syscalls.hpp"
#include "pman/async/task.hpp"
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace pman::async {

IoUringLoop::IoUringLoop(const EventLoopConfig& config) {
    io_uring_params params{};
    params.flags = 0;

    ring_.ring_fd = io_uring_setup(256, &params);
    if (ring_.ring_fd < 0) {
        throw std::runtime_error("Failed to setup io_uring");
    }

    ring_.sq_size = params.sq_off.array + params.sq_entries * sizeof(uint32_t);
    ring_.cq_size = params.cq_off.cqes + params.cq_entries * sizeof(io_uring_cqe);

    ring_.sq_ptr = mmap(nullptr, ring_.sq_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, ring_.ring_fd, IORING_OFF_SQ_RING);
    if (ring_.sq_ptr == MAP_FAILED) {
        close(ring_.ring_fd);
        throw std::runtime_error("Failed to mmap submission queue");
    }

    ring_.sqes = static_cast<io_uring_sqe*>(
        mmap(nullptr, params.sq_entries * sizeof(io_uring_sqe),
             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
             ring_.ring_fd, IORING_OFF_SQES));
    if (ring_.sqes == MAP_FAILED) {
        munmap(ring_.sq_ptr, ring_.sq_size);
        close(ring_.ring_fd);
        throw std::runtime_error("Failed to mmap submission queue entries");
    }

    ring_.cq_ptr = mmap(nullptr, ring_.cq_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_POPULATE, ring_.ring_fd, IORING_OFF_CQ_RING);
    if (ring_.cq_ptr == MAP_FAILED) {
        munmap(ring_.sqes, params.sq_entries * sizeof(io_uring_sqe));
        munmap(ring_.sq_ptr, ring_.sq_size);
        close(ring_.ring_fd);
        throw std::runtime_error("Failed to mmap completion queue");
    }

    ring_.sq_head = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.sq_ptr) + params.sq_off.head);
    ring_.sq_tail = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.sq_ptr) + params.sq_off.tail);
    ring_.sq_mask = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.sq_ptr) + params.sq_off.ring_mask);
    ring_.sq_entries = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.sq_ptr) + params.sq_off.ring_entries);
    ring_.sq_array = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.sq_ptr) + params.sq_off.array);

    ring_.cq_head = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.cq_ptr) + params.cq_off.head);
    ring_.cq_tail = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.cq_ptr) + params.cq_off.tail);
    ring_.cq_mask = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.cq_ptr) + params.cq_off.ring_mask);
    ring_.cq_entries = reinterpret_cast<uint32_t*>(static_cast<char*>(ring_.cq_ptr) + params.cq_off.ring_entries);
    ring_.cqes = reinterpret_cast<io_uring_cqe*>(static_cast<char*>(ring_.cq_ptr) + params.cq_off.cqes);

    eventFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd_ < 0) {
        munmap(ring_.cq_ptr, ring_.cq_size);
        munmap(ring_.sqes, params.sq_entries * sizeof(io_uring_sqe));
        munmap(ring_.sq_ptr, ring_.sq_size);
        close(ring_.ring_fd);
        throw std::runtime_error("Failed to create eventfd");
    }
}

IoUringLoop::~IoUringLoop() {
    if (eventFd_ >= 0) {
        close(eventFd_);
    }
    if (ring_.cq_ptr && ring_.cq_ptr != MAP_FAILED) {
        munmap(ring_.cq_ptr, ring_.cq_size);
    }
    if (ring_.sqes && ring_.sqes != MAP_FAILED) {
        munmap(ring_.sqes, *ring_.sq_entries * sizeof(io_uring_sqe));
    }
    if (ring_.sq_ptr && ring_.sq_ptr != MAP_FAILED) {
        munmap(ring_.sq_ptr, ring_.sq_size);
    }
    if (ring_.ring_fd >= 0) {
        close(ring_.ring_fd);
    }
}

void IoUringLoop::run() {
    running_ = true;

    while (running_) {
        processTimers();
        submitTimerOp();
        submitSqes();

        int ret = io_uring_enter(ring_.ring_fd, 0, 1, IORING_ENTER_GETEVENTS, nullptr);
        if (ret < 0 && errno != EINTR) {
            throw std::runtime_error("io_uring_enter failed");
        }

        processCqes();

        // Process posted callbacks at end of iteration
        processPostedCallbacks();
    }
}

void IoUringLoop::stop() {
    running_ = false;
    uint64_t val = 1;
    write(eventFd_, &val, sizeof(val));
}

uint64_t IoUringLoop::addTimer(std::chrono::nanoseconds duration, std::function<void()> callback) {
    uint64_t id = nextTimerId_++;
    auto expiry = std::chrono::steady_clock::now() + duration;

    TimerData timer{id, expiry};
    timers_.push(timer);
    timerCallbacks_[id] = std::move(callback);
    timerPeriods_[id] = std::chrono::nanoseconds(0);

    return id;
}

uint64_t IoUringLoop::addPeriodicTimer(std::chrono::nanoseconds period, std::function<void()> callback) {
    uint64_t id = nextTimerId_++;
    auto expiry = std::chrono::steady_clock::now() + period;

    TimerData timer{id, expiry};
    timers_.push(timer);
    timerCallbacks_[id] = std::move(callback);
    timerPeriods_[id] = period;

    return id;
}

bool IoUringLoop::cancelTimer(uint64_t timerId) {
    auto erased1 = timerCallbacks_.erase(timerId);
    timerPeriods_.erase(timerId);
    return erased1 > 0;
}

bool IoUringLoop::runOnce(std::chrono::nanoseconds timeout) {
    processTimers();
    submitTimerOp();
    submitSqes();

    int ret = io_uring_enter(ring_.ring_fd, 0, 1, IORING_ENTER_GETEVENTS, nullptr);
    if (ret < 0 && errno != EINTR) {
        return false;
    }

    processCqes();
    return true;
}

bool IoUringLoop::isRunning() const noexcept {
    return running_;
}

BackendType IoUringLoop::backend() const noexcept {
    return BackendType::IoUring;
}

void IoUringLoop::addFd(int fd, Event events, EventCallback callback) {
    // Stub implementation
}

void IoUringLoop::modifyFd(int fd, Event events) {
    // Stub implementation
}

void IoUringLoop::removeFd(int fd) {
    // Stub implementation
}

void IoUringLoop::post(std::function<void()> callback) {
    postedCallbacks_.push_back(std::move(callback));
    // Wake up the event loop
    uint64_t val = 1;
    write(eventFd_, &val, sizeof(val));
}

void IoUringLoop::processPostedCallbacks() {
    if (postedCallbacks_.empty()) {
        return;
    }

    // Move callbacks to local vector to avoid issues if callbacks post more callbacks
    std::vector<std::function<void()>> callbacks;
    callbacks.swap(postedCallbacks_);

    for (auto& callback : callbacks) {
        if (callback) {
            detail::TrampolineGuard guard;
            callback();
        }
    }
}

size_t IoUringLoop::activeFds() const noexcept {
    return 0;
}

size_t IoUringLoop::activeTimers() const noexcept {
    return timerCallbacks_.size();
}

void IoUringLoop::processTimers() {
    auto now = std::chrono::steady_clock::now();

    while (!timers_.empty()) {
        const auto& timer = timers_.top();

        if (timer.expiry > now) {
            break;
        }

        auto callbackIt = timerCallbacks_.find(timer.id);
        auto periodIt = timerPeriods_.find(timer.id);

        if (callbackIt == timerCallbacks_.end() || periodIt == timerPeriods_.end()) {
            timers_.pop();
            continue;
        }

        // CRITICAL FIX: Copy callback BEFORE any map modifications
        auto callback = callbackIt->second;
        auto period = periodIt->second;

        timers_.pop();

        // Reschedule periodic timer or cleanup one-shot
        if (period.count() > 0) {
            TimerData nextTimer{timer.id, now + period};
            timers_.push(nextTimer);
        } else {
            timerCallbacks_.erase(timer.id);
            timerPeriods_.erase(timer.id);
        }

        // Invoke callback last with trampoline guard
        if (callback) {
            detail::TrampolineGuard guard;
            callback();
        }
    }
}

void IoUringLoop::submitTimerOp() {
    if (timers_.empty() || timerOpPending_) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    const auto& nextTimer = timers_.top();

    if (nextTimer.expiry <= now) {
        return;
    }

    auto duration = nextTimer.expiry - now;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    currentTimeout_.tv_sec = ns / 1000000000;
    currentTimeout_.tv_nsec = ns % 1000000000;

    io_uring_sqe* sqe = getSqe();
    if (!sqe) {
        return;
    }

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->addr = reinterpret_cast<uint64_t>(&currentTimeout_);
    sqe->len = 1;
    sqe->user_data = 0;

    timerOpPending_ = true;
}

io_uring_sqe* IoUringLoop::getSqe() {
    uint32_t tail = *ring_.sq_tail;
    uint32_t next = tail + 1;
    uint32_t head = __atomic_load_n(ring_.sq_head, __ATOMIC_ACQUIRE);

    if (next - head > *ring_.sq_entries) {
        return nullptr;
    }

    io_uring_sqe* sqe = &ring_.sqes[tail & *ring_.sq_mask];
    ring_.sq_array[tail & *ring_.sq_mask] = tail & *ring_.sq_mask;

    return sqe;
}

void IoUringLoop::submitSqes() {
    uint32_t tail = *ring_.sq_tail;
    if (tail != *ring_.sq_tail) {
        __atomic_store_n(ring_.sq_tail, tail, __ATOMIC_RELEASE);
    }
}

void IoUringLoop::processCqes() {
    uint32_t head = *ring_.cq_head;
    uint32_t tail = __atomic_load_n(ring_.cq_tail, __ATOMIC_ACQUIRE);

    while (head != tail) {
        io_uring_cqe* cqe = &ring_.cqes[head & *ring_.cq_mask];

        if (cqe->user_data == 0) {
            timerOpPending_ = false;
        } else {
            auto it = userDataCallbacks_.find(cqe->user_data);
            if (it != userDataCallbacks_.end()) {
                detail::TrampolineGuard guard;
                it->second(cqe->res);
                userDataCallbacks_.erase(it);
            }
        }

        head++;
    }

    __atomic_store_n(ring_.cq_head, head, __ATOMIC_RELEASE);
}

} // namespace pman::async
