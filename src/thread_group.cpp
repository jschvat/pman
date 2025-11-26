#include "pman/thread_group.hpp"

namespace pman {

ManagedThread& ThreadGroup::add(std::string name, std::function<void()> task, ThreadAttributes attributes) {
    return threads_.emplace_back(std::move(name), std::move(task), std::move(attributes));
}

void ThreadGroup::joinAll() {
    for (auto& thread : threads_) {
        thread.join();
    }
}

void ThreadGroup::detachAll() {
    for (auto& thread : threads_) {
        thread.detach();
    }
}

void ThreadGroup::setAffinityAll(const CpuSet& set) {
    for (auto& thread : threads_) {
        thread.setCpuAffinity(set);
    }
}

void ThreadGroup::setSchedulingAll(SchedulingPolicy policy, int priority) {
    for (auto& thread : threads_) {
        thread.setScheduling(policy, priority);
    }
}

}  // namespace pman
