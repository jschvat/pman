#pragma once

#include <cstddef>
#include <string>

namespace pman {

enum class SharedMemoryMode {
    Create,
    Open,
    OpenOrCreate,
};

struct SharedMemoryOptions {
    SharedMemoryMode mode{SharedMemoryMode::OpenOrCreate};
    int permissions{0600};
    bool hugePages{false};
    bool lockInMemory{false};
};

class SharedMemory {
public:
    SharedMemory(std::string name, std::size_t size, SharedMemoryOptions options = {});
    ~SharedMemory();

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    SharedMemory(SharedMemory&&) noexcept;
    SharedMemory& operator=(SharedMemory&&) noexcept;

    [[nodiscard]] void* data() noexcept { return ptr_; }
    [[nodiscard]] const void* data() const noexcept { return ptr_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    template <typename T>
    [[nodiscard]] T* as() noexcept {
        return static_cast<T*>(ptr_);
    }

    template <typename T>
    [[nodiscard]] const T* as() const noexcept {
        return static_cast<const T*>(ptr_);
    }

    void flush();
    void flush(std::size_t offset, std::size_t length);

    static void unlink(const std::string& name);
    static bool exists(const std::string& name);

    static SharedMemory createAnonymous(std::size_t size, bool hugePages = false);

private:
    SharedMemory() = default;

    std::string name_;
    void* ptr_{nullptr};
    std::size_t size_{0};
    int fd_{-1};
    bool owner_{false};
};

}  // namespace pman
