#pragma once

#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include <string>
#include <vector>
#include <cstddef>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <system_error>
#include <optional>

namespace pman::async {

/// @brief Socket address wrapper
struct SocketAddr {
    sockaddr_in addr{};

    SocketAddr() {
        addr.sin_family = AF_INET;
    }

    SocketAddr(const std::string& ip, uint16_t port) {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            throw std::runtime_error("Invalid IP address");
        }
    }

    static SocketAddr any(uint16_t port) {
        SocketAddr sa;
        sa.addr.sin_addr.s_addr = INADDR_ANY;
        sa.addr.sin_port = htons(port);
        return sa;
    }

    std::string ip() const {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        return buf;
    }

    uint16_t port() const {
        return ntohs(addr.sin_port);
    }
};

/// @brief Async TCP socket
///
/// Provides async TCP socket operations integrated with the event loop.
/// Uses edge-triggered epoll for efficient I/O multiplexing.
///
/// Example:
/// @code
/// // Server
/// auto server = []() -> Task<void> {
///     auto listener = AsyncSocket::bind("0.0.0.0", 8080);
///     co_await listener.listen(10);
///
///     auto client = co_await listener.accept();
///     auto data = co_await client.recv(1024);
///     co_await client.send("Hello!");
/// };
///
/// // Client
/// auto client = []() -> Task<void> {
///     auto sock = co_await AsyncSocket::connect("127.0.0.1", 8080);
///     co_await sock.send("Hello server!");
///     auto response = co_await sock.recv(1024);
/// };
/// @endcode
class AsyncSocket {
public:
    /// Create a TCP socket
    static AsyncSocket create() {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "socket failed");
        }
        return AsyncSocket(fd);
    }

    /// Bind socket to address
    static AsyncSocket bind(const std::string& ip, uint16_t port) {
        auto sock = create();
        SocketAddr addr(ip, port);

        // Allow address reuse
        int opt = 1;
        setsockopt(sock.fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (::bind(sock.fd_, (sockaddr*)&addr.addr, sizeof(addr.addr)) < 0) {
            throw std::system_error(errno, std::system_category(), "bind failed");
        }

        return sock;
    }

    /// Connect to remote address
    static Task<std::optional<AsyncSocket>> connect(const std::string& ip, uint16_t port, EventLoop* loop = nullptr) {
        if (!loop) {
            loop = EventLoop::current();
        }

        try {
            auto sock = create();
            SocketAddr addr(ip, port);

            int ret = ::connect(sock.fd_, (sockaddr*)&addr.addr, sizeof(addr.addr));

            // Non-blocking connect returns EINPROGRESS
            if (ret < 0 && errno != EINPROGRESS) {
                co_return std::nullopt;
            }

            // Wait for connection to complete
            if (errno == EINPROGRESS) {
                co_await sock.waitWritable(loop);

                // Check for connection errors
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(sock.fd_, SOL_SOCKET, SO_ERROR, &error, &len);

                if (error != 0) {
                    co_return std::nullopt;
                }
            }

            co_return std::move(sock);

        } catch (const std::system_error&) {
            co_return std::nullopt;
        }
    }

    /// Create from existing fd
    explicit AsyncSocket(int fd) : fd_(fd) {}

    AsyncSocket(AsyncSocket&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    AsyncSocket& operator=(AsyncSocket&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    ~AsyncSocket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    AsyncSocket(const AsyncSocket&) = delete;
    AsyncSocket& operator=(const AsyncSocket&) = delete;

    /// Listen for incoming connections
    Task<IoResult<int>> listen(int backlog = 10) {
        int ret = ::listen(fd_, backlog);

        IoResult<int> result;
        if (ret < 0) {
            result.error = errno;
        } else {
            result.value = ret;
        }

        co_await yieldNow();
        co_return result;
    }

    /// Accept incoming connection
    Task<std::optional<AsyncSocket>> accept(EventLoop* loop = nullptr) {
        if (!loop) {
            loop = EventLoop::current();
        }

        // Wait for connection to be available
        co_await waitReadable(loop);

        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = ::accept4(fd_, (sockaddr*)&client_addr, &addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (client_fd < 0) {
            co_return std::nullopt;
        }

        co_return AsyncSocket(client_fd);
    }

    /// Send data
    Task<IoResult<ssize_t>> send(const void* data, size_t size, EventLoop* loop = nullptr) {
        if (!loop) {
            loop = EventLoop::current();
        }

        ssize_t total = 0;
        const char* ptr = static_cast<const char*>(data);

        while (total < static_cast<ssize_t>(size)) {
            ssize_t n = ::send(fd_, ptr + total, size - total, MSG_NOSIGNAL);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    co_await waitWritable(loop);
                    continue;
                }
                co_return IoResult<ssize_t>{total, errno};
            }

            total += n;
        }

        co_return IoResult<ssize_t>{total, 0};
    }

    /// Send string
    Task<IoResult<ssize_t>> send(const std::string& data, EventLoop* loop = nullptr) {
        return send(data.data(), data.size(), loop);
    }

    /// Send vector
    Task<IoResult<ssize_t>> send(const std::vector<char>& data, EventLoop* loop = nullptr) {
        return send(data.data(), data.size(), loop);
    }

    /// Receive data
    Task<IoResult<std::vector<char>>> recv(size_t max_size, EventLoop* loop = nullptr) {
        if (!loop) {
            loop = EventLoop::current();
        }

        std::vector<char> buffer(max_size);

        // Wait for data to be available
        co_await waitReadable(loop);

        ssize_t n = ::recv(fd_, buffer.data(), max_size, 0);

        IoResult<std::vector<char>> result;
        if (n < 0) {
            result.error = errno;
        } else if (n == 0) {
            // Connection closed
            result.value = std::vector<char>();
        } else {
            buffer.resize(n);
            result.value = std::move(buffer);
        }

        co_return result;
    }

    /// Close socket
    Task<IoResult<int>> close() {
        if (fd_ < 0) {
            co_return IoResult<int>{0, 0};
        }

        int ret = ::close(fd_);
        int saved_errno = errno;

        if (ret == 0) {
            fd_ = -1;
        }

        IoResult<int> result;
        if (ret < 0) {
            result.error = saved_errno;
        } else {
            result.value = ret;
        }

        co_await yieldNow();
        co_return result;
    }

    int fd() const { return fd_; }
    bool isOpen() const { return fd_ >= 0; }

private:
    /// Wait for socket to be readable
    Task<void> waitReadable(EventLoop* loop) {
        struct Awaiter {
            int fd;
            EventLoop* loop;

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) {
                loop->addFd(fd, Event::Read, [h, this](Event) {
                    loop->removeFd(fd);
                    h.resume();
                });
            }

            void await_resume() {}
        };

        co_await Awaiter{fd_, loop};
    }

    /// Wait for socket to be writable
    Task<void> waitWritable(EventLoop* loop) {
        struct Awaiter {
            int fd;
            EventLoop* loop;

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> h) {
                loop->addFd(fd, Event::Write, [h, this](Event) {
                    loop->removeFd(fd);
                    h.resume();
                });
            }

            void await_resume() {}
        };

        co_await Awaiter{fd_, loop};
    }

    int fd_{-1};
};

} // namespace pman::async
