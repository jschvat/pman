#include <iostream>
#include <thread>
#include <vector>

#include "pman/barrier.hpp"
#include "pman/rw_spinlock.hpp"
#include "pman/seqlock.hpp"

int main() {
    pman::SeqLock seqlock;
    int sharedValue = 0;
    std::thread writer([&]() {
        for (int i = 0; i < 5; ++i) {
            seqlock.lock();
            sharedValue = i;
            std::cout << "writer -> " << i << "\n";
            seqlock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    });

    std::thread reader([&]() {
        for (int i = 0; i < 5; ++i) {
            int snapshot = seqlock.read([&]() { return sharedValue; });
            std::cout << "reader sees " << snapshot << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    writer.join();
    reader.join();

    pman::RwSpinLock rw;
    int counter = 0;
    auto readerFn = [&](int id) {
        rw.lock_shared();
        std::cout << "rw reader " << id << " sees " << counter << "\n";
        rw.unlock_shared();
    };
    auto writerFn = [&]() {
        rw.lock();
        counter += 10;
        std::cout << "rw writer bumped counter to " << counter << "\n";
        rw.unlock();
    };

    std::thread r1(readerFn, 1);
    std::thread r2(readerFn, 2);
    std::thread w(writerFn);
    r1.join();
    r2.join();
    w.join();

    pman::Barrier barrier(3);
    auto barrierWorker = [&](int id) {
        std::cout << "worker " << id << " waiting...\n";
        barrier.wait();
        std::cout << "worker " << id << " released!\n";
    };
    std::thread b1(barrierWorker, 1);
    std::thread b2(barrierWorker, 2);
    std::thread b3(barrierWorker, 3);
    b1.join();
    b2.join();
    b3.join();

    return 0;
}
