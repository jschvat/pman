#include <mutex>
#include <vector>

#include <sys/wait.h>

#include "pman/diagnostics.hpp"
#include "pman/process.hpp"
#include "pman/thread.hpp"

int main() {
    std::mutex mutex;
    std::vector<pman::ThreadEventType> threadEvents;
    pman::setThreadObserver([&](const pman::ThreadEvent& event) {
        std::lock_guard<std::mutex> lock(mutex);
        threadEvents.push_back(event.type);
    });

    {
        pman::ManagedThread thread("diag-thread", [] {});
        thread.join();
    }

    if (threadEvents.size() < 3 ||
        threadEvents[0] != pman::ThreadEventType::Created ||
        threadEvents[1] != pman::ThreadEventType::Started ||
        threadEvents[2] != pman::ThreadEventType::Finished) {
        return 1;
    }

    std::vector<pman::ProcessEventType> processEvents;
    pman::setProcessObserver([&](const pman::ProcessEvent& event) {
        std::lock_guard<std::mutex> lock(mutex);
        processEvents.push_back(event.type);
    });

    pman::ProcessConfig cfg{
        .executable = "/bin/true",
    };
    auto handle = pman::launchProcess(cfg);
    int status = handle.wait();
    if (!WIFEXITED(status)) {
        return 2;
    }

    if (processEvents.size() < 3 ||
        processEvents[0] != pman::ProcessEventType::LaunchAttempt ||
        processEvents[1] != pman::ProcessEventType::LaunchSuccess ||
        processEvents.back() != pman::ProcessEventType::Exit) {
        return 3;
    }

    pman::setThreadObserver(nullptr);
    pman::setProcessObserver(nullptr);

    return 0;
}
