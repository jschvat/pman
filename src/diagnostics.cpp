#include "pman/diagnostics.hpp"

#include <mutex>
#include <utility>

namespace pman {
namespace {

std::mutex observerMutex;
ThreadObserver threadObserver;
ProcessObserver processObserver;

template <typename Observer, typename Event>
void dispatchEvent(Observer& observer, const Event& event) {
    Observer copy;
    {
        std::lock_guard<std::mutex> lock(observerMutex);
        copy = observer;
    }
    if (copy) {
        copy(event);
    }
}

}  // namespace

void setThreadObserver(ThreadObserver observer) {
    std::lock_guard<std::mutex> lock(observerMutex);
    threadObserver = std::move(observer);
}

void setProcessObserver(ProcessObserver observer) {
    std::lock_guard<std::mutex> lock(observerMutex);
    processObserver = std::move(observer);
}

namespace detail {

void emitThreadEvent(const ThreadEvent& event) {
    dispatchEvent(threadObserver, event);
}

void emitProcessEvent(const ProcessEvent& event) {
    dispatchEvent(processObserver, event);
}

}  // namespace detail

}  // namespace pman
