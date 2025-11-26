#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "pman/futex.hpp"
#include "pman/thread.hpp"
#include "pman/thread_attributes.hpp"

namespace pman {

class StopRequestedException : public std::exception {
public:
    const char* what() const noexcept override {
        return "Thread stop was requested";
    }
};

struct PauseControl {
    std::atomic<bool> pauseRequested{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> paused{false};
    FutexMutex mutex;
    FutexCondVar pauseCv;
    FutexCondVar resumeCv;
};

class PauseToken {
public:
    void checkPause();

    [[nodiscard]] bool stopRequested() const noexcept;
    [[nodiscard]] bool pauseRequested() const noexcept;

private:
    friend class PausableThread;
    explicit PauseToken(std::shared_ptr<PauseControl> control);

    std::shared_ptr<PauseControl> control_;
};

class PausableThread {
public:
    PausableThread() = default;
    PausableThread(std::string name, std::function<void(PauseToken&)> task,
                   ThreadAttributes attributes = {});
    ~PausableThread();

    PausableThread(const PausableThread&) = delete;
    PausableThread& operator=(const PausableThread&) = delete;

    PausableThread(PausableThread&&) noexcept;
    PausableThread& operator=(PausableThread&&) noexcept;

    void pause();
    void resume();
    void requestStop();

    [[nodiscard]] bool isPaused() const noexcept;
    [[nodiscard]] bool isStopRequested() const noexcept;
    [[nodiscard]] bool joinable() const noexcept;

    void join();
    void detach();

    void setCpuAffinity(const CpuSet& set) const;
    void setScheduling(SchedulingPolicy policy, int priority);

    [[nodiscard]] pthread_t nativeHandle() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;

private:
    ManagedThread thread_;
    std::shared_ptr<PauseControl> control_;
    std::string name_;
};

}  // namespace pman
