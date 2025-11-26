#pragma once

#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <signal.h>

namespace pman::async {

inline int io_uring_setup(unsigned entries, io_uring_params* params) {
    return syscall(__NR_io_uring_setup, entries, params);
}

inline int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                          unsigned flags, sigset_t* sig) {
    return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, _NSIG / 8);
}

inline int io_uring_register(int fd, unsigned opcode, void* arg, unsigned nr_args) {
    return syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

} // namespace pman::async
