#include "pman/thread.hpp"

#include "pman/diagnostics.hpp"
#include "pman/topology.hpp"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace pman {
namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void throwSystemError(int err, const char* what) {
    throw std::system_error(err, std::generic_category(), what);
}

}  // namespace

ManagedThread::ManagedThread(std::string name, std::function<void()> task, ThreadAttributes attributes)
    : name_(std::move(name)), attributes_(std::move(attributes)) {
    if (!task) {
        throw std::invalid_argument("ManagedThread requires a callable");
    }
    start(std::move(task));
}

ManagedThread::~ManagedThread() {
    if (started_ && joinable_) {
        pthread_detach(thread_);
    }
}

ManagedThread::ManagedThread(ManagedThread&& other) noexcept {
    *this = std::move(other);
}

ManagedThread& ManagedThread::operator=(ManagedThread&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (joinable_) {
        pthread_detach(thread_);
    }

    thread_ = other.thread_;
    joinable_ = other.joinable_;
    started_ = other.started_;
    name_ = std::move(other.name_);
    attributes_ = std::move(other.attributes_);
    state_ = std::move(other.state_);

    other.joinable_ = false;
    other.started_ = false;
    other.thread_ = pthread_t{};
    other.state_.reset();

    return *this;
}

void ManagedThread::start(std::function<void()> task) {
    state_ = std::make_shared<detail::ThreadState>();
    state_->task = std::move(task);
    state_->name = name_;

    if (!attributes_.affinity && attributes_.numaNode) {
        try {
            attributes_.affinity = SystemTopology::instance().cpuSetForNode(*attributes_.numaNode);
        } catch (...) {
            // Ignore failures; fallback to OS defaults.
        }
    }

    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        throwSystemError(rc, "pthread_attr_init");
    }
    struct AttrGuard {
        pthread_attr_t* attrPtr;
        explicit AttrGuard(pthread_attr_t* ptr) : attrPtr(ptr) {}
        ~AttrGuard() {
            if (attrPtr) {
                pthread_attr_destroy(attrPtr);
            }
        }
    } guard(&attr);

    if (attributes_.stackSize) {
        rc = pthread_attr_setstacksize(&attr, *attributes_.stackSize);
        if (rc != 0) {
            throwSystemError(rc, "pthread_attr_setstacksize");
        }
    }

    if (attributes_.guardSize) {
        rc = pthread_attr_setguardsize(&attr, *attributes_.guardSize);
        if (rc != 0) {
            throwSystemError(rc, "pthread_attr_setguardsize");
        }
    }

    if (attributes_.policy || attributes_.priority) {
        rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        if (rc != 0) {
            throwSystemError(rc, "pthread_attr_setinheritsched");
        }

        if (attributes_.policy) {
            rc = pthread_attr_setschedpolicy(&attr, static_cast<int>(*attributes_.policy));
            if (rc != 0) {
                throwSystemError(rc, "pthread_attr_setschedpolicy");
            }
        }

        sched_param param{};
        param.sched_priority = attributes_.priority.value_or(0);
        rc = pthread_attr_setschedparam(&attr, &param);
        if (rc != 0) {
            throwSystemError(rc, "pthread_attr_setschedparam");
        }
    }

    if (attributes_.affinity) {
#ifdef __linux__
        rc = pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), attributes_.affinity->data());
        if (rc != 0) {
            throwSystemError(rc, "pthread_attr_setaffinity_np");
        }
#endif
    }

    if (attributes_.detached) {
        rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (rc != 0) {
            throwSystemError(rc, "pthread_attr_setdetachstate");
        }
    }

    auto stateHolder = new std::shared_ptr<detail::ThreadState>(state_);
    rc = pthread_create(&thread_, &attr, &ManagedThread::entryPoint, stateHolder);
    if (rc != 0) {
        delete stateHolder;
        state_.reset();
        throwSystemError(rc, "pthread_create");
    }

    started_ = true;
    joinable_ = !attributes_.detached;

    detail::emitThreadEvent(ThreadEvent{
        .type = ThreadEventType::Created,
        .name = name_,
        .handle = thread_,
        .timestamp = Clock::now(),
    });

    if (!name_.empty()) {
        std::string truncated = name_.substr(0, 15);
        pthread_setname_np(thread_, truncated.c_str());
    }
}

void* ManagedThread::entryPoint(void* data) noexcept {
    std::unique_ptr<std::shared_ptr<detail::ThreadState>> holder(
        static_cast<std::shared_ptr<detail::ThreadState>*>(data));
    auto state = *holder;
    if (!state || !state->task) {
        return nullptr;
    }

    detail::emitThreadEvent(ThreadEvent{
        .type = ThreadEventType::Started,
        .name = state->name,
        .handle = pthread_self(),
        .timestamp = Clock::now(),
    });

    bool threw = false;
    try {
        state->task();
    } catch (...) {
        state->exception = std::current_exception();
        threw = true;
        detail::emitThreadEvent(ThreadEvent{
            .type = ThreadEventType::Exception,
            .name = state->name,
            .handle = pthread_self(),
            .timestamp = Clock::now(),
            .message = "unhandled exception in ManagedThread task",
        });
    }

    detail::emitThreadEvent(ThreadEvent{
        .type = ThreadEventType::Finished,
        .name = state->name,
        .handle = pthread_self(),
        .timestamp = Clock::now(),
        .message = threw ? "completed with exception" : "",
    });

    return nullptr;
}

void ManagedThread::join() {
    if (!joinable_) {
        return;
    }

    int rc = pthread_join(thread_, nullptr);
    if (rc != 0) {
        throwSystemError(rc, "pthread_join");
    }

    joinable_ = false;

    std::exception_ptr ex;
    if (state_) {
        ex = state_->exception;
    }
    cleanupState();

    if (ex) {
        std::rethrow_exception(ex);
    }
}

void ManagedThread::detach() {
    if (!joinable_) {
        return;
    }

    int rc = pthread_detach(thread_);
    if (rc != 0) {
        throwSystemError(rc, "pthread_detach");
    }
    joinable_ = false;
    cleanupState();
}

void ManagedThread::setCpuAffinity(const CpuSet& set) const {
#ifdef __linux__
    if (!started_) {
        throw std::logic_error("thread has not been started");
    }
    int rc = pthread_setaffinity_np(thread_, sizeof(cpu_set_t), set.data());
    if (rc != 0) {
        throwSystemError(rc, "pthread_setaffinity_np");
    }
#else
    (void)set;
#endif
}

void ManagedThread::setScheduling(SchedulingPolicy policy, int priority) {
    if (!started_) {
        throw std::logic_error("thread has not been started");
    }
    sched_param param{};
    param.sched_priority = priority;
    int rc = pthread_setschedparam(thread_, static_cast<int>(policy), &param);
    if (rc != 0) {
        throwSystemError(rc, "pthread_setschedparam");
    }
}

void ManagedThread::setName(std::string_view name) {
    name_ = std::string(name);
    if (started_) {
        std::string truncated = name_.substr(0, 15);
        pthread_setname_np(thread_, truncated.c_str());
    }
}

void ManagedThread::cleanupState() {
    state_.reset();
}

}  // namespace pman
