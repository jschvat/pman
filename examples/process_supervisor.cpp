#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include "pman/process_builder.hpp"

int main() {
    try {
        std::cout << "Launching /bin/echo via ProcessBuilder...\n";
        pman::ProcessBuilder echoBuilder{"/bin/echo"};
        echoBuilder.args({"hello", "from", "pman"});
        auto pipe = echoBuilder.captureStdout();
        auto echoHandle = echoBuilder.spawn();

        char buffer[128];
        ssize_t bytes = ::read(pipe.readEnd(), buffer, sizeof(buffer));
        std::string output(buffer, buffer + std::max<ssize_t>(bytes, 0));
        std::cout << "child output: " << output;
        echoHandle.wait();

        std::cout << "Starting long-running task (/bin/sleep 5) and terminating after 1s...\n";
        pman::ProcessBuilder sleepBuilder{"/bin/sleep"};
        sleepBuilder.arg("5");
        auto sleepHandle = sleepBuilder.spawn();
        bool graceful = sleepHandle.terminate(std::chrono::seconds(1));
        std::cout << "terminate() returned " << std::boolalpha << graceful << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "process supervisor example failed: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
