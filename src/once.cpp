#include "pman/once.hpp"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <limits>
#include <system_error>

namespace pman {
namespace {

void futex_wait(std::atomic<int>* addr, int expected) {
    while (true) {
        int rc = syscall(SYS_futex,
            reinterpret_cast<int*>(addr),
            FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
            expected,
            nullptr,
            nullptr,
            0);
        if (rc == 0 || errno == EAGAIN) {
            return;
        }
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "futex_wait");
        }
    }
}

void futex_wake_all(std::atomic<int>* addr) {
    syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
        std::numeric_limits<int>::max(),
        nullptr,
        nullptr,
        0);
}

}  // namespace

bool FutexOnceFlag::has_run() const noexcept {
    return state_.load(std::memory_order_acquire) == Done;
}

void FutexOnceFlag::wait_for_completion() {
    while (true) {
        int state = state_.load(std::memory_order_acquire);
        if (state == Done) {
            return;
        }
        if (state == NotStarted) {
            return;
        }
        futex_wait(&state_, Running);
    }
}

void FutexOnceFlag::mark_done_and_wake() {
    state_.store(Done, std::memory_order_release);
    futex_wake_all(&state_);
}

}  // namespace pman
