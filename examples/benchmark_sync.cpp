#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <shared_mutex>
#include <thread>

#include "pman/seqlock.hpp"

namespace {

struct Stats {
    double opsPerSec{0.0};
};

template <typename Runner>
Stats runBenchmark(const char* label, Runner&& runner) {
    std::cout << "Running " << label << " benchmark..." << std::endl;
    auto stats = runner();
    std::cout << "  " << std::fixed << std::setprecision(2) << stats.opsPerSec << " ops/sec" << std::endl;
    return stats;
}

Stats benchmarkSeqlock(std::chrono::milliseconds duration) {
    pman::SeqLock lock;
    int sharedValue = 0;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> reads{0};

    std::thread writer([&]() {
        while (!stop.load()) {
            lock.lock();
            ++sharedValue;
            lock.unlock();
            std::this_thread::yield();
        }
    });

    std::thread reader([&]() {
        while (!stop.load()) {
            lock.read([&]() { (void)sharedValue; });
            ++reads;
        }
    });

    std::this_thread::sleep_for(duration);
    stop = true;
    writer.join();
    reader.join();

    double opsPerSec = static_cast<double>(reads.load()) / (duration.count() / 1000.0);
    return Stats{opsPerSec};
}

Stats benchmarkSharedMutex(std::chrono::milliseconds duration) {
    std::shared_mutex mutex;
    int sharedValue = 0;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> reads{0};

    std::thread writer([&]() {
        while (!stop.load()) {
            std::unique_lock lk(mutex);
            ++sharedValue;
            lk.unlock();
            std::this_thread::yield();
        }
    });

    std::thread reader([&]() {
        while (!stop.load()) {
            std::shared_lock lk(mutex);
            (void)sharedValue;
            ++reads;
        }
    });

    std::this_thread::sleep_for(duration);
    stop = true;
    writer.join();
    reader.join();

    double opsPerSec = static_cast<double>(reads.load()) / (duration.count() / 1000.0);
    return Stats{opsPerSec};
}

}  // namespace

int main() {
    constexpr auto duration = std::chrono::milliseconds(500);
    auto seqStats = runBenchmark("pman::SeqLock", [=] { return benchmarkSeqlock(duration); });
    auto sharedStats = runBenchmark("std::shared_mutex", [=] { return benchmarkSharedMutex(duration); });

    std::cout << "SeqLock speedup vs shared_mutex: "
              << (sharedStats.opsPerSec > 0 ? seqStats.opsPerSec / sharedStats.opsPerSec : 0.0)
              << "x" << std::endl;
    return 0;
}
