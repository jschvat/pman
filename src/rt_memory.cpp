#include "pman/rt_memory.hpp"

#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace pman {

namespace {

[[noreturn]] void throwErrno(const char* context) {
    throw std::system_error(errno, std::generic_category(), context);
}

int pageSize() {
    static const int size = static_cast<int>(::sysconf(_SC_PAGESIZE));
    return size > 0 ? size : 4096;
}

}  // namespace

void lockMemory(const MemoryLockOptions& options) {
    int flags = 0;
    if (options.lockCurrent) {
        flags |= MCL_CURRENT;
    }
    if (options.lockFuture) {
        flags |= MCL_FUTURE;
    }
    if (flags == 0) {
        return;
    }
    if (::mlockall(flags) != 0) {
        throwErrno("mlockall");
    }
}

void unlockMemory() {
    if (::munlockall() != 0) {
        throwErrno("munlockall");
    }
}

void adviseWillNeed(void* addr, std::size_t length) {
    if (::madvise(addr, length, MADV_WILLNEED) != 0) {
        throwErrno("madvise(MADV_WILLNEED)");
    }
}

void enableHugePages(void* addr, std::size_t length) {
    if (::madvise(addr, length, MADV_HUGEPAGE) != 0) {
        throwErrno("madvise(MADV_HUGEPAGE)");
    }
}

void prefaultStack(std::size_t bytes) {
    if (bytes == 0) {
        return;
    }
    int size = pageSize();
    bytes = (bytes + size - 1) & ~(static_cast<std::size_t>(size) - 1);
    std::vector<char> buffer(bytes, 0);
    volatile char* ptr = buffer.data();
    for (std::size_t i = 0; i < bytes; i += size) {
        ptr[i] = 0;
    }
}

void setGuardSize(std::size_t guardBytes) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        throwErrno("pthread_attr_init");
    }
    if (pthread_attr_setguardsize(&attr, guardBytes) != 0) {
        pthread_attr_destroy(&attr);
        throwErrno("pthread_attr_setguardsize");
    }
    pthread_attr_destroy(&attr);
}

}  // namespace pman
