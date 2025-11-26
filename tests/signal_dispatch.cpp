#include <atomic>
#include <chrono>
#include <thread>

#include <pthread.h>
#include <signal.h>

#include "pman/signal.hpp"

int main() {
#ifdef __linux__
    auto& dispatcher = pman::SignalDispatcher::instance();
    dispatcher.start({SIGUSR1});

    std::atomic<int> handled{0};
    dispatcher.registerHandler(SIGUSR1, [&](const signalfd_siginfo& info) {
        handled.store(static_cast<int>(info.ssi_signo), std::memory_order_relaxed);
    });

    sigset_t current;
    pthread_sigmask(SIG_SETMASK, nullptr, &current);
    if (!sigismember(&current, SIGUSR1)) {
        dispatcher.stop();
        return 5;
    }

    ::kill(getpid(), SIGUSR1);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (handled.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    dispatcher.stop();
    if (handled.load(std::memory_order_relaxed) != SIGUSR1) {
        return 1;
    }

    auto ok = pman::sendSignalSequence(getpid(), {{SIGUSR1, std::chrono::milliseconds(0)}}, false);
    return ok ? 0 : 2;
#else
    return 0;
#endif
}
