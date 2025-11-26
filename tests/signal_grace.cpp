#include <chrono>
#include <sys/wait.h>

#include "pman/process_builder.hpp"
#include "pman/signal.hpp"

int main() {
#ifdef __linux__
    pman::ProcessBuilder builder("/bin/sleep");
    builder.arg("5");
    auto handle = builder.spawn();

    pman::GracefulShutdownOptions opts;
    opts.termTimeout = std::chrono::milliseconds(500);
    opts.intTimeout = std::chrono::milliseconds(500);
    opts.hupTimeout = std::chrono::milliseconds(500);
    bool graceful = pman::gracefulShutdown(handle.pid(), opts);

    int status = handle.wait();
    if (!graceful) {
        return 1;
    }
    if (!WIFSIGNALED(status) && !WIFEXITED(status)) {
        return 2;
    }
#endif
    return 0;
}
