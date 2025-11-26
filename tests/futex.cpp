#include <chrono>
#include <thread>
#include <vector>

#include "pman/futex.hpp"

int main() {
    pman::FutexMutex mutex;
    pman::FutexCondVar cv;
    bool ready = false;
    int value = 0;

    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        pman::FutexLockGuard guard(mutex);
        value = 42;
        ready = true;
        cv.notify_one();
    });

    {
        pman::FutexLockGuard guard(mutex);
        while (!ready) {
            cv.wait(mutex);
        }
        if (value != 42) {
            return 1;
        }
    }

    producer.join();
    return 0;
}
