#include <atomic>
#include <thread>
#include <vector>

#include "pman/seqlock.hpp"
#include "pman/rw_spinlock.hpp"
#include "pman/barrier.hpp"

int main() {
    pman::SeqLock seqlock;
    int value = 0;
    std::atomic<bool> done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 100; ++i) {
            seqlock.lock();
            value = i;
            seqlock.unlock();
        }
        done = true;
    });

    std::thread reader([&]() {
        while (!done.load()) {
            seqlock.read([&]() { int snapshot = value; (void)snapshot; });
        }
    });

    writer.join();
    reader.join();

    pman::RwSpinLock rwlock;
    int counter = 0;
    std::thread w([&]() {
        rwlock.lock();
        counter = 42;
        rwlock.unlock();
    });
    std::thread r([&]() {
        rwlock.lock_shared();
        int snapshot = counter;
        (void)snapshot;
        rwlock.unlock_shared();
    });
    w.join();
    r.join();

    pman::Barrier barrier(2);
    std::atomic<int> barrierHits{0};
    std::thread b1([&]() {
        barrier.wait();
        barrierHits++;
    });
    std::thread b2([&]() {
        barrier.wait();
        barrierHits++;
    });
    b1.join();
    b2.join();

    return barrierHits == 2 ? 0 : 1;
}
