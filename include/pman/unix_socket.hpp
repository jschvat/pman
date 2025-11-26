#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <sys/types.h>

namespace pman {

enum class UnixSocketType {
    Stream,
    Datagram,
    SeqPacket,
};

class UnixSocket {
public:
    explicit UnixSocket(UnixSocketType type);
    ~UnixSocket();

    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;

    UnixSocket(UnixSocket&&) noexcept;
    UnixSocket& operator=(UnixSocket&&) noexcept;

    void bind(const std::string& path);
    void listen(int backlog = 128);
    UnixSocket accept();

    void connect(const std::string& path);

    std::size_t send(std::span<const std::byte> data, int flags = 0);
    std::size_t recv(std::span<std::byte> buffer, int flags = 0);

    std::size_t sendTo(std::span<const std::byte> data, const std::string& path);
    std::size_t recvFrom(std::span<std::byte> buffer, std::string& senderPath);

    void sendFd(int fd);
    int recvFd();
    void sendFds(std::span<const int> fds);
    std::vector<int> recvFds(std::size_t maxFds);

    void setNonBlocking(bool enabled);
    void setReceiveTimeout(std::chrono::nanoseconds timeout);
    void setSendTimeout(std::chrono::nanoseconds timeout);
    void setPassCredentials(bool enabled);

    struct Credentials {
        pid_t pid;
        uid_t uid;
        gid_t gid;
    };
    [[nodiscard]] Credentials peerCredentials() const;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    int releaseFd();

    static void unlink(const std::string& path);

protected:
    UnixSocket(int fd, UnixSocketType type);

private:
    int fd_{-1};
    UnixSocketType type_;
    std::string boundPath_;
};

class AbstractUnixSocket : public UnixSocket {
public:
    using UnixSocket::UnixSocket;

    void bind(const std::string& name);
    void connect(const std::string& name);
};

}  // namespace pman
