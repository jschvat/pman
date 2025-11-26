#include "pman/unix_socket.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <system_error>

namespace pman {

namespace {

int socketTypeToNative(UnixSocketType type) {
    switch (type) {
    case UnixSocketType::Stream:
        return SOCK_STREAM;
    case UnixSocketType::Datagram:
        return SOCK_DGRAM;
    case UnixSocketType::SeqPacket:
        return SOCK_SEQPACKET;
    }
    return SOCK_STREAM;
}

}  // namespace

UnixSocket::UnixSocket(UnixSocketType type) : type_(type) {
    fd_ = ::socket(AF_UNIX, socketTypeToNative(type) | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
}

UnixSocket::UnixSocket(int fd, UnixSocketType type) : fd_(fd), type_(type) {}

UnixSocket::~UnixSocket() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    if (!boundPath_.empty()) {
        ::unlink(boundPath_.c_str());
    }
}

UnixSocket::UnixSocket(UnixSocket&& other) noexcept
    : fd_(other.fd_),
      type_(other.type_),
      boundPath_(std::move(other.boundPath_)) {
    other.fd_ = -1;
    other.boundPath_.clear();
}

UnixSocket& UnixSocket::operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        if (!boundPath_.empty()) {
            ::unlink(boundPath_.c_str());
        }

        fd_ = other.fd_;
        type_ = other.type_;
        boundPath_ = std::move(other.boundPath_);

        other.fd_ = -1;
        other.boundPath_.clear();
    }
    return *this;
}

void UnixSocket::bind(const std::string& path) {
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    if (path.size() >= sizeof(addr.sun_path)) {
        throw std::invalid_argument("Unix socket path too long");
    }

    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    ::unlink(path.c_str());

    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind");
    }

    boundPath_ = path;
}

void UnixSocket::listen(int backlog) {
    if (::listen(fd_, backlog) < 0) {
        throw std::system_error(errno, std::generic_category(), "listen");
    }
}

UnixSocket UnixSocket::accept() {
    int clientFd = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (clientFd < 0) {
        throw std::system_error(errno, std::generic_category(), "accept");
    }
    return UnixSocket(clientFd, type_);
}

void UnixSocket::connect(const std::string& path) {
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    if (path.size() >= sizeof(addr.sun_path)) {
        throw std::invalid_argument("Unix socket path too long");
    }

    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::system_error(errno, std::generic_category(), "connect");
    }
}

std::size_t UnixSocket::send(std::span<const std::byte> data, int flags) {
    ssize_t n = ::send(fd_, data.data(), data.size(), flags | MSG_NOSIGNAL);
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(), "send");
    }
    return static_cast<std::size_t>(n);
}

std::size_t UnixSocket::recv(std::span<std::byte> buffer, int flags) {
    ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), flags);
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(), "recv");
    }
    return static_cast<std::size_t>(n);
}

std::size_t UnixSocket::sendTo(std::span<const std::byte> data, const std::string& path) {
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    ssize_t n = ::sendto(fd_, data.data(), data.size(), MSG_NOSIGNAL,
                          reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(), "sendto");
    }
    return static_cast<std::size_t>(n);
}

std::size_t UnixSocket::recvFrom(std::span<std::byte> buffer, std::string& senderPath) {
    struct sockaddr_un addr{};
    socklen_t addrLen = sizeof(addr);

    ssize_t n = ::recvfrom(fd_, buffer.data(), buffer.size(), 0,
                            reinterpret_cast<struct sockaddr*>(&addr), &addrLen);
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(), "recvfrom");
    }

    senderPath = addr.sun_path;
    return static_cast<std::size_t>(n);
}

void UnixSocket::sendFd(int fd) {
    std::span<const int> fds(&fd, 1);
    sendFds(fds);
}

int UnixSocket::recvFd() {
    auto fds = recvFds(1);
    if (fds.empty()) {
        throw std::runtime_error("No file descriptor received");
    }
    return fds[0];
}

void UnixSocket::sendFds(std::span<const int> fds) {
    char dummy = '\0';

    struct iovec iov{};
    iov.iov_base = &dummy;
    iov.iov_len = 1;

    std::size_t cmsgSize = CMSG_SPACE(fds.size() * sizeof(int));
    std::vector<char> cmsgBuf(cmsgSize);

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf.data();
    msg.msg_controllen = cmsgSize;

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), fds.data(), fds.size() * sizeof(int));

    if (::sendmsg(fd_, &msg, MSG_NOSIGNAL) < 0) {
        throw std::system_error(errno, std::generic_category(), "sendmsg");
    }
}

std::vector<int> UnixSocket::recvFds(std::size_t maxFds) {
    char dummy;

    struct iovec iov{};
    iov.iov_base = &dummy;
    iov.iov_len = 1;

    std::size_t cmsgSize = CMSG_SPACE(maxFds * sizeof(int));
    std::vector<char> cmsgBuf(cmsgSize);

    struct msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgBuf.data();
    msg.msg_controllen = cmsgSize;

    ssize_t n = ::recvmsg(fd_, &msg, 0);
    if (n < 0) {
        throw std::system_error(errno, std::generic_category(), "recvmsg");
    }

    std::vector<int> result;

    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            std::size_t fdCount = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int* fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
            for (std::size_t i = 0; i < fdCount; ++i) {
                result.push_back(fds[i]);
            }
        }
    }

    return result;
}

void UnixSocket::setNonBlocking(bool enabled) {
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl");
    }

    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    if (::fcntl(fd_, F_SETFL, flags) < 0) {
        throw std::system_error(errno, std::generic_category(), "fcntl");
    }
}

void UnixSocket::setReceiveTimeout(std::chrono::nanoseconds timeout) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(timeout - secs);

    struct timeval tv{};
    tv.tv_sec = secs.count();
    tv.tv_usec = usecs.count();

    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt");
    }
}

void UnixSocket::setSendTimeout(std::chrono::nanoseconds timeout) {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(timeout - secs);

    struct timeval tv{};
    tv.tv_sec = secs.count();
    tv.tv_usec = usecs.count();

    if (::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt");
    }
}

void UnixSocket::setPassCredentials(bool enabled) {
    int val = enabled ? 1 : 0;
    if (::setsockopt(fd_, SOL_SOCKET, SO_PASSCRED, &val, sizeof(val)) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt");
    }
}

UnixSocket::Credentials UnixSocket::peerCredentials() const {
    struct ucred cred{};
    socklen_t len = sizeof(cred);

    if (::getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        throw std::system_error(errno, std::generic_category(), "getsockopt");
    }

    return Credentials{cred.pid, cred.uid, cred.gid};
}

int UnixSocket::releaseFd() {
    int fd = fd_;
    fd_ = -1;
    boundPath_.clear();
    return fd;
}

void UnixSocket::unlink(const std::string& path) {
    ::unlink(path.c_str());
}

void AbstractUnixSocket::bind(const std::string& name) {
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    if (name.size() >= sizeof(addr.sun_path) - 1) {
        throw std::invalid_argument("Abstract socket name too long");
    }

    addr.sun_path[0] = '\0';
    std::strncpy(addr.sun_path + 1, name.c_str(), sizeof(addr.sun_path) - 2);

    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + name.size();

    if (::bind(fd(), reinterpret_cast<struct sockaddr*>(&addr), len) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind");
    }
}

void AbstractUnixSocket::connect(const std::string& name) {
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    if (name.size() >= sizeof(addr.sun_path) - 1) {
        throw std::invalid_argument("Abstract socket name too long");
    }

    addr.sun_path[0] = '\0';
    std::strncpy(addr.sun_path + 1, name.c_str(), sizeof(addr.sun_path) - 2);

    socklen_t len = offsetof(struct sockaddr_un, sun_path) + 1 + name.size();

    if (::connect(fd(), reinterpret_cast<struct sockaddr*>(&addr), len) < 0) {
        throw std::system_error(errno, std::generic_category(), "connect");
    }
}

}  // namespace pman
