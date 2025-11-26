#include "pman/pipe.hpp"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <system_error>

namespace pman {
namespace {

Pipe makePipe(bool cloexec) {
    int fds[2];
#if defined(__linux__)
    int flags = cloexec ? O_CLOEXEC : 0;
    if (::pipe2(fds, flags) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe2");
    }
#else
    if (::pipe(fds) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    if (cloexec) {
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
#endif
    return Pipe(fds[0], fds[1]);
}

}  // namespace

Pipe Pipe::create(bool cloexec) {
    return makePipe(cloexec);
}

Pipe::Pipe(int readFd, int writeFd) : readFd_(readFd), writeFd_(writeFd) {}

Pipe::Pipe(Pipe&& other) noexcept {
    *this = std::move(other);
}

Pipe& Pipe::operator=(Pipe&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    close();
    readFd_ = other.readFd_;
    writeFd_ = other.writeFd_;
    other.readFd_ = -1;
    other.writeFd_ = -1;
    return *this;
}

Pipe::~Pipe() {
    close();
}

int Pipe::releaseRead() {
    int fd = readFd_;
    readFd_ = -1;
    return fd;
}

int Pipe::releaseWrite() {
    int fd = writeFd_;
    writeFd_ = -1;
    return fd;
}

void Pipe::closeRead() {
    if (readFd_ >= 0) {
        ::close(readFd_);
        readFd_ = -1;
    }
}

void Pipe::closeWrite() {
    if (writeFd_ >= 0) {
        ::close(writeFd_);
        writeFd_ = -1;
    }
}

void Pipe::close() {
    closeRead();
    closeWrite();
}

}  // namespace pman
