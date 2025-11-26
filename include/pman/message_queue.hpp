#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "pman/shared_memory.hpp"

namespace pman {

class PosixMessageQueue {
public:
    struct Options {
        int maxMessages;
        std::size_t maxMessageSize;
        int permissions;
        bool blocking;

        Options() : maxMessages(10), maxMessageSize(8192), permissions(0600), blocking(true) {}
    };

    PosixMessageQueue(std::string name, SharedMemoryMode mode, Options options = Options());
    ~PosixMessageQueue();

    PosixMessageQueue(const PosixMessageQueue&) = delete;
    PosixMessageQueue& operator=(const PosixMessageQueue&) = delete;

    PosixMessageQueue(PosixMessageQueue&&) noexcept;
    PosixMessageQueue& operator=(PosixMessageQueue&&) noexcept;

    void send(std::span<const std::byte> data, unsigned int priority = 0);
    bool trySend(std::span<const std::byte> data, unsigned int priority = 0);

    template <typename Rep, typename Period>
    bool sendFor(std::span<const std::byte> data,
                 const std::chrono::duration<Rep, Period>& timeout,
                 unsigned int priority = 0);

    std::size_t receive(std::span<std::byte> buffer, unsigned int* priority = nullptr);
    std::optional<std::size_t> tryReceive(std::span<std::byte> buffer, unsigned int* priority = nullptr);

    template <typename Rep, typename Period>
    std::optional<std::size_t> receiveFor(std::span<std::byte> buffer,
                                           const std::chrono::duration<Rep, Period>& timeout,
                                           unsigned int* priority = nullptr);

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] std::size_t maxMessageSize() const noexcept { return maxSize_; }
    [[nodiscard]] long currentMessages() const;

    static void unlink(const std::string& name);

private:
    bool sendImpl(std::span<const std::byte> data, unsigned int priority, const timespec* timeout);
    std::optional<std::size_t> receiveImpl(std::span<std::byte> buffer, unsigned int* priority,
                                            const timespec* timeout);

    std::string name_;
    int mqd_{-1};
    std::size_t maxSize_{0};
};

template <std::size_t MaxMessageSize, std::size_t Capacity>
class SharedRingBuffer {
public:
    SharedRingBuffer();

    SharedRingBuffer(const SharedRingBuffer&) = delete;
    SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;

    bool push(std::span<const std::byte> data);
    void pushBlocking(std::span<const std::byte> data);

    std::optional<std::size_t> pop(std::span<std::byte> buffer);
    std::size_t popBlocking(std::span<std::byte> buffer);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    static constexpr std::size_t max_message_size() noexcept { return MaxMessageSize; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    struct alignas(64) Slot {
        std::atomic<std::uint32_t> ready{0};
        std::uint32_t size{0};
        std::byte data[MaxMessageSize];
    };

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::atomic<int> notEmpty_{0};
    alignas(64) std::atomic<int> notFull_{0};
    Slot slots_[Capacity];
};

template <typename Rep, typename Period>
bool PosixMessageQueue::sendFor(std::span<const std::byte> data,
                                 const std::chrono::duration<Rep, Period>& timeout,
                                 unsigned int priority) {
    auto now = std::chrono::system_clock::now();
    auto deadline = now + timeout;
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(deadline);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - secs);

    timespec ts;
    ts.tv_sec = secs.time_since_epoch().count();
    ts.tv_nsec = ns.count();

    return sendImpl(data, priority, &ts);
}

template <typename Rep, typename Period>
std::optional<std::size_t> PosixMessageQueue::receiveFor(
    std::span<std::byte> buffer,
    const std::chrono::duration<Rep, Period>& timeout,
    unsigned int* priority) {
    auto now = std::chrono::system_clock::now();
    auto deadline = now + timeout;
    auto secs = std::chrono::time_point_cast<std::chrono::seconds>(deadline);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - secs);

    timespec ts;
    ts.tv_sec = secs.time_since_epoch().count();
    ts.tv_nsec = ns.count();

    return receiveImpl(buffer, priority, &ts);
}

}  // namespace pman
