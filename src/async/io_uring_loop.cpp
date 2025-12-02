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

    ring_.ring_fd = io_uring_setup(config.io_uring_entries > 0 ? config.io_uring_entries : 256, &params);
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
        // Process ready timers first
        processTimers();

        // Submit a timeout for next timer if we have one pending
        submitTimerOp();

        // Submit any pending SQEs - memory barrier
        submitSqes();

        // Determine how to wait
        uint32_t to_submit = sqPendingCount_;
        sqPendingCount_ = 0;

        // If we have no timers and nothing to submit, just do a short poll
        unsigned int flags = 0;
        unsigned int min_complete = 0;

        if (to_submit > 0 || timerOpPending_) {
            // We have pending work, wait for at least one completion
            flags = IORING_ENTER_GETEVENTS;
            min_complete = 1;
        }

        int ret = io_uring_enter(ring_.ring_fd, to_submit, min_complete, flags, nullptr);

        if (ret < 0 && errno != EINTR && errno != ETIME) {
            // ETIME is expected when timeout expires
            if (errno != ETIME) {
                throw std::runtime_error("io_uring_enter failed");
            }
        }

        // Process completions
        processCqes();

        // Process posted callbacks at end of iteration
        processPostedCallbacks();

        // If no timers and nothing pending, avoid spinning
        if (timersByExpiry_.empty() && postedCallbacks_.empty() && !timerOpPending_) {
            break;
        }
    }
}

void IoUringLoop::stop() {
    running_ = false;
    uint64_t val = 1;
    [[maybe_unused]] auto r = write(eventFd_, &val, sizeof(val));
}

uint64_t IoUringLoop::addTimer(std::chrono::nanoseconds duration, std::function<void()> callback) {
    uint64_t id = nextTimerId_++;
    auto expiry = std::chrono::steady_clock::now() + duration;

    // Phase 2: Insert into multimap and store iterator for O(1) cancellation
    TimerEntry entry{id, std::move(callback), std::chrono::nanoseconds(0)};
    auto it = timersByExpiry_.emplace(expiry, std::move(entry));
    timerById_[id] = it;

    return id;
}

uint64_t IoUringLoop::addPeriodicTimer(std::chrono::nanoseconds period, std::function<void()> callback) {
    uint64_t id = nextTimerId_++;
    auto expiry = std::chrono::steady_clock::now() + period;

    // Phase 2: Insert into multimap with period info
    TimerEntry entry{id, std::move(callback), period};
    auto it = timersByExpiry_.emplace(expiry, std::move(entry));
    timerById_[id] = it;

    return id;
}

bool IoUringLoop::cancelTimer(uint64_t timerId) {
    // Phase 2: O(1) cancellation via iterator lookup
    auto it = timerById_.find(timerId);
    if (it == timerById_.end()) {
        return false;
    }

    timersByExpiry_.erase(it->second);
    timerById_.erase(it);
    return true;
}

bool IoUringLoop::runOnce(std::chrono::nanoseconds timeout) {
    processTimers();
    submitTimerOp();
    submitSqes();

    int ret = io_uring_enter(ring_.ring_fd, sqPendingCount_, 1, IORING_ENTER_GETEVENTS, nullptr);
    sqPendingCount_ = 0;

    if (ret < 0 && errno != EINTR && errno != ETIME) {
        return false;
    }

    processCqes();
    processPostedCallbacks();
    return true;
}

bool IoUringLoop::isRunning() const noexcept {
    return running_;
}

BackendType IoUringLoop::backend() const noexcept {
    return BackendType::IoUring;
}

void IoUringLoop::addFd(int fd, Event events, EventCallback callback) {
    // TODO: Implement FD monitoring with io_uring
}

void IoUringLoop::modifyFd(int fd, Event events) {
    // TODO: Implement FD modification
}

void IoUringLoop::removeFd(int fd) {
    // TODO: Implement FD removal
}

void IoUringLoop::post(std::function<void()> callback) {
    postedCallbacks_.push_back(std::move(callback));
    // Wake up the event loop
    uint64_t val = 1;
    [[maybe_unused]] auto r = write(eventFd_, &val, sizeof(val));
}

// PHASE 3: Inline hot-path function with branch prediction hints
inline void IoUringLoop::processPostedCallbacks() {
    if (postedCallbacks_.empty()) [[likely]] {
        return;
    }

    // Move callbacks to local vector to avoid issues if callbacks post more callbacks
    std::vector<std::function<void()>> callbacks;
    callbacks.swap(postedCallbacks_);

    for (auto& callback : callbacks) {
        if (callback) [[likely]] {
            detail::TrampolineGuard guard;
            callback();
        }
    }
}

size_t IoUringLoop::activeFds() const noexcept {
    return 0;
}

size_t IoUringLoop::activeTimers() const noexcept {
    return timerById_.size();
}

void IoUringLoop::processTimers() {
    auto now = std::chrono::steady_clock::now();

    // Phase 2: Batch callbacks for better throughput
    std::vector<std::function<void()>> batch;
    batch.reserve(32);

    // Phase 2: Collect timers to reschedule (can't modify map while iterating)
    std::vector<std::pair<TimePoint, TimerEntry>> toReschedule;

    // Phase 2: Iterate from beginning (earliest) until we hit unexpired timer
    auto it = timersByExpiry_.begin();
    while (it != timersByExpiry_.end()) {
        if (it->first > now) {
            break;  // All remaining timers are in the future
        }

        const auto& entry = it->second;
        uint64_t timerId = entry.id;

        // Copy callback before modifying containers
        auto callback = entry.callback;
        auto period = entry.period;

        // Remove from id lookup
        timerById_.erase(timerId);

        // If periodic, schedule for reschedule (can't insert while iterating)
        if (period.count() > 0) {
            TimerEntry newEntry{timerId, callback, period};
            toReschedule.emplace_back(now + period, std::move(newEntry));
        }

        // Batch the callback
        if (callback) {
            batch.push_back(std::move(callback));
        }

        // Erase and advance
        it = timersByExpiry_.erase(it);
    }

    // Phase 2: Reschedule periodic timers
    for (auto& [expiry, entry] : toReschedule) {
        uint64_t id = entry.id;
        auto newIt = timersByExpiry_.emplace(expiry, std::move(entry));
        timerById_[id] = newIt;
    }

    // Execute all callbacks in batch (better cache locality)
    detail::TrampolineGuard guard;
    for (auto& callback : batch) {
        callback();
    }
}

// PHASE 3: Inline hot-path function with branch prediction hints
inline void IoUringLoop::submitTimerOp() {
    if (timersByExpiry_.empty() || timerOpPending_) [[unlikely]] {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    const auto& nextExpiry = timersByExpiry_.begin()->first;

    if (nextExpiry <= now) [[unlikely]] {
        return;  // Timer already ready, will be processed in processTimers
    }

    auto duration = nextExpiry - now;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    currentTimeout_.tv_sec = ns / 1000000000;
    currentTimeout_.tv_nsec = ns % 1000000000;

    io_uring_sqe* sqe = getSqe();
    if (!sqe) [[unlikely]] {
        return;
    }

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_TIMEOUT;
    sqe->addr = reinterpret_cast<uint64_t>(&currentTimeout_);
    sqe->len = 1;
    sqe->user_data = 0;  // Timer completions use user_data = 0

    timerOpPending_ = true;
}

// PHASE 3: Inline hot-path function with branch prediction hints
inline io_uring_sqe* IoUringLoop::getSqe() {
    uint32_t head = __atomic_load_n(ring_.sq_head, __ATOMIC_ACQUIRE);
    uint32_t tail = *ring_.sq_tail;

    // Check if queue is full
    if (tail - head >= *ring_.sq_entries) [[unlikely]] {
        return nullptr;
    }

    uint32_t index = tail & *ring_.sq_mask;
    io_uring_sqe* sqe = &ring_.sqes[index];
    ring_.sq_array[index] = index;

    // Advance tail for next getSqe call
    *ring_.sq_tail = tail + 1;
    sqPendingCount_++;

    return sqe;
}

// PHASE 3: Inline hot-path function
inline void IoUringLoop::submitSqes() {
    // Memory barrier to ensure SQE writes are visible before we update tail
    __atomic_store_n(ring_.sq_tail, *ring_.sq_tail, __ATOMIC_RELEASE);
}

void IoUringLoop::processCqes() {
    uint32_t head = *ring_.cq_head;
    uint32_t tail = __atomic_load_n(ring_.cq_tail, __ATOMIC_ACQUIRE);

    while (head != tail) {
        io_uring_cqe* cqe = &ring_.cqes[head & *ring_.cq_mask];

        if (cqe->user_data == 0) {
            // Timer completion
            timerOpPending_ = false;
        } else {
            // User FD/operation completion
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
