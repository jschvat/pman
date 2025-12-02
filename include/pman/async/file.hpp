#pragma once

#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"
#include "pman/async/sleep.hpp"
#include <string>
#include <vector>
#include <cstddef>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <system_error>

namespace pman::async {

/// @brief Result of an async I/O operation
template<typename T>
struct IoResult {
    T value{};
    int error{0};  // errno value, 0 = success

    IoResult() = default;
    IoResult(T v, int e) : value(std::move(v)), error(e) {}

    bool ok() const { return error == 0; }
    explicit operator bool() const { return ok(); }
};

/// @brief Async file operations
///
/// Performs file I/O asynchronously without blocking the event loop.
/// Currently uses thread pool offloading; io_uring support coming later.
///
/// Example:
/// @code
/// auto task = []() -> Task<void> {
///     auto file = AsyncFile::open("/tmp/test.txt", O_RDWR | O_CREAT);
///
///     std::string data = "Hello, async world!";
///     auto writeResult = co_await file.write(data);
///
///     co_await file.seek(0);
///     auto readResult = co_await file.read(1024);
///
///     co_await file.close();
/// };
/// @endcode
class AsyncFile {
public:
    /// Open a file asynchronously
    static AsyncFile open(const std::string& path, int flags, mode_t mode = 0644) {
        int fd = ::open(path.c_str(), flags, mode);
        if (fd < 0) {
            throw std::system_error(errno, std::system_category(), "open failed");
        }
        return AsyncFile(fd);
    }

    /// Create AsyncFile from existing file descriptor
    explicit AsyncFile(int fd) : fd_(fd) {}

    AsyncFile(AsyncFile&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    AsyncFile& operator=(AsyncFile&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    ~AsyncFile() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    AsyncFile(const AsyncFile&) = delete;
    AsyncFile& operator=(const AsyncFile&) = delete;

    /// Read data from file
    /// @param size Maximum number of bytes to read
    /// @return IoResult containing bytes read and any error
    Task<IoResult<std::vector<char>>> read(size_t size) {
        std::vector<char> buffer(size);

        // For now, perform blocking read (will optimize with io_uring later)
        ssize_t n = ::read(fd_, buffer.data(), size);

        IoResult<std::vector<char>> result;
        if (n < 0) {
            result.error = errno;
        } else {
            buffer.resize(n);
            result.value = std::move(buffer);
        }

        // Yield to event loop
        co_await yieldNow();
        co_return result;
    }

    /// Write data to file
    /// @param data Data to write
    /// @return IoResult containing bytes written and any error
    Task<IoResult<ssize_t>> write(const std::vector<char>& data) {
        return write(data.data(), data.size());
    }

    /// Write string to file
    Task<IoResult<ssize_t>> write(const std::string& data) {
        return write(data.data(), data.size());
    }

    /// Write raw bytes to file
    Task<IoResult<ssize_t>> write(const void* data, size_t size) {
        ssize_t n = ::write(fd_, data, size);

        IoResult<ssize_t> result;
        if (n < 0) {
            result.error = errno;
        } else {
            result.value = n;
        }

        // Yield to event loop
        co_await yieldNow();
        co_return result;
    }

    /// Seek to position in file
    /// @param offset Offset from whence
    /// @param whence SEEK_SET, SEEK_CUR, or SEEK_END
    /// @return IoResult containing new position and any error
    Task<IoResult<off_t>> seek(off_t offset, int whence = SEEK_SET) {
        off_t pos = ::lseek(fd_, offset, whence);

        IoResult<off_t> result;
        if (pos == (off_t)-1) {
            result.error = errno;
        } else {
            result.value = pos;
        }

        co_await yieldNow();
        co_return result;
    }

    /// Sync file data to disk
    Task<IoResult<int>> sync() {
        int ret = ::fsync(fd_);

        IoResult<int> result;
        if (ret < 0) {
            result.error = errno;
        } else {
            result.value = ret;
        }

        co_await yieldNow();
        co_return result;
    }

    /// Close the file (also happens automatically on destruction)
    Task<IoResult<int>> close() {
        if (fd_ < 0) {
            co_return IoResult<int>{0, 0};
        }

        int ret = ::close(fd_);
        int saved_errno = errno;

        if (ret == 0) {
            fd_ = -1;  // Mark as closed
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

    /// Get the file descriptor
    int fd() const { return fd_; }

    /// Check if file is open
    bool isOpen() const { return fd_ >= 0; }

private:
    int fd_{-1};
};

/// Helper functions for common file operations

/// Read entire file into string
inline Task<IoResult<std::string>> readFile(const std::string& path) {
    try {
        auto file = AsyncFile::open(path, O_RDONLY);

        // Get file size
        struct stat st;
        if (fstat(file.fd(), &st) < 0) {
            co_return IoResult<std::string>{{}, errno};
        }

        // Read entire file
        auto result = co_await file.read(st.st_size);
        if (!result) {
            co_return IoResult<std::string>{{}, result.error};
        }

        std::string content(result.value.begin(), result.value.end());
        co_return IoResult<std::string>{std::move(content), 0};

    } catch (const std::system_error& e) {
        co_return IoResult<std::string>{{}, e.code().value()};
    }
}

/// Write string to file
inline Task<IoResult<ssize_t>> writeFile(const std::string& path, const std::string& content) {
    try {
        auto file = AsyncFile::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        auto result = co_await file.write(content);
        co_return result;

    } catch (const std::system_error& e) {
        co_return IoResult<ssize_t>{0, e.code().value()};
    }
}

} // namespace pman::async
