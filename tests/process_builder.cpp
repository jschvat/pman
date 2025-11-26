#include <array>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "pman/process_builder.hpp"

int main() {
    // Capture stdout from /bin/echo
    pman::ProcessBuilder builder("/bin/echo");
    builder.arg("hello");
    auto pipe = builder.captureStdout();
    auto handle = builder.spawn();
    char buffer[64];
    ssize_t n = ::read(pipe.readEnd(), buffer, sizeof(buffer));
    if (n <= 0) {
        return 1;
    }
    std::string output(buffer, buffer + n);
    if (output.find("hello") == std::string::npos) {
        return 2;
    }
    int status = handle.wait();
    if (!WIFEXITED(status)) {
        return 3;
    }

    // posix_spawn path for /bin/echo
    pman::ProcessBuilder posix("/bin/echo");
    posix.arg("posix");
    posix.usePosixSpawn(true);
    auto posixPipe = posix.captureStdout();
    auto posixHandle = posix.spawn();
    char posixBuf[32];
    ssize_t posixRead = ::read(posixPipe.readEnd(), posixBuf, sizeof(posixBuf));
    if (posixRead <= 0 || std::string(posixBuf, posixBuf + posixRead).find("posix") == std::string::npos) {
        return 4;
    }
    posixHandle.wait();

    // Capture stdin/stdout via pipes using /bin/cat
    pman::ProcessBuilder cat("/bin/cat");
    auto stdinPipe = cat.captureStdin();
    auto stdoutPipe = cat.captureStdout();
    auto catHandle = cat.spawn();
    const char* payload = "ping from builder\n";
    int writeFd = stdinPipe.releaseWrite();
    if (writeFd < 0) {
        return 5;
    }
    if (::write(writeFd, payload, std::strlen(payload)) <= 0) {
        return 5;
    }
    ::close(writeFd);
    char outBuf[64];
    ssize_t outBytes = ::read(stdoutPipe.readEnd(), outBuf, sizeof(outBuf));
    if (outBytes <= 0 || std::string(outBuf, outBuf + outBytes).find("ping") == std::string::npos) {
        return 6;
    }
    catHandle.wait();

    // Launch a long-running process and ensure terminate works with timeout
    pman::ProcessBuilder sleeper("/bin/sleep");
    sleeper.arg("5");
    auto sleepy = sleeper.spawn();
    bool graceful = sleepy.terminate(std::chrono::milliseconds(200));
    if (!graceful) {
        // process should be killed by SIGKILL, but handle must be reaped
        if (sleepy.valid()) {
            return 7;
        }
    }

    return 0;
}
