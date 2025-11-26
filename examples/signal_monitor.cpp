#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <unistd.h>

#include "pman/signal.hpp"

int main() {
#ifdef __linux__
    auto& dispatcher = pman::SignalDispatcher::instance();
    dispatcher.start({SIGUSR1});

    dispatcher.registerHandler(SIGUSR1, [](const signalfd_siginfo& info) {
        std::cout << "Received signal " << info.ssi_signo << " from pid " << info.ssi_pid << "\n";
    });

    dispatcher.blockSignalsForCurrentThread();
    std::cout << "Raising SIGUSR1 after 250ms...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    pman::sendSignalSequence(getpid(), {{SIGUSR1, std::chrono::nanoseconds(0)}}, false);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    dispatcher.stop();
#else
    std::cout << "Signal example requires Linux signalfd" << std::endl;
#endif
    return 0;
}
