#pragma once

#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"
#include <optional>
#include <utility>
#include <memory>
#include <deque>
#include <coroutine>
#include <atomic>

namespace pman::async {

/// @brief Internal implementation of a bounded MPMC channel
///
/// Shared between all Sender and Receiver handles.
/// Thread-safe for multi-producer, multi-consumer scenarios.
template<typename T>
class ChannelImpl {
public:
    explicit ChannelImpl(size_t capacity)
        : capacity_(capacity)
        , closed_(false) {}

    ~ChannelImpl() {
        close();
    }

    /// Send a value to the channel
    /// Blocks if channel is full until space is available or channel is closed
    struct SendAwaiter {
        ChannelImpl<T>* channel;
        T value;
        bool sent{false};

        bool await_ready() const noexcept {
            // Check if we can send immediately
            if (channel->closed_.load(std::memory_order_acquire)) {
                return true;  // Channel closed, fail immediately
            }
            return channel->buffer_.size() < channel->capacity_;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            // Add to waiting senders queue
            channel->sendWaiters_.push_back({handle, std::move(value)});
        }

        bool await_resume() {
            if (channel->closed_.load(std::memory_order_acquire)) {
                return false;  // Channel was closed
            }

            // If we were woken up, the value was already moved into buffer
            return sent;
        }
    };

    /// Receive a value from the channel
    /// Blocks if channel is empty until data is available or channel is closed
    struct RecvAwaiter {
        ChannelImpl<T>* channel;
        std::optional<T> result;

        bool await_ready() const noexcept {
            // Check if we can receive immediately
            return !channel->buffer_.empty() ||
                   channel->closed_.load(std::memory_order_acquire);
        }

        void await_suspend(std::coroutine_handle<> handle) {
            // Add to waiting receivers queue
            channel->recvWaiters_.push_back(handle);
        }

        std::optional<T> await_resume() {
            if (!channel->buffer_.empty()) {
                T value = std::move(channel->buffer_.front());
                channel->buffer_.pop_front();

                // Wake up a waiting sender if any
                channel->wakeNextSender();

                return value;
            }

            // Channel closed and empty
            return std::nullopt;
        }
    };

    SendAwaiter sendAwaiter(T value) {
        return SendAwaiter{this, std::move(value), false};
    }

    RecvAwaiter recvAwaiter() {
        return RecvAwaiter{this, std::nullopt};
    }

    /// Try to send without blocking
    bool trySend(T value) {
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }

        if (buffer_.size() >= capacity_) {
            return false;
        }

        buffer_.push_back(std::move(value));
        wakeNextReceiver();
        return true;
    }

    /// Try to receive without blocking
    std::optional<T> tryRecv() {
        if (buffer_.empty()) {
            return std::nullopt;
        }

        T value = std::move(buffer_.front());
        buffer_.pop_front();
        wakeNextSender();
        return value;
    }

    /// Close the channel
    /// Wakes all waiting senders and receivers
    void close() {
        bool expected = false;
        if (closed_.compare_exchange_strong(expected, true, std::memory_order_release)) {
            // Wake all waiting receivers
            for (auto handle : recvWaiters_) {
                resumeHandle(handle);
            }
            recvWaiters_.clear();

            // Wake all waiting senders (they will see channel is closed)
            for (auto& waiter : sendWaiters_) {
                resumeHandle(waiter.handle);
            }
            sendWaiters_.clear();
        }
    }

    bool isClosed() const {
        return closed_.load(std::memory_order_acquire);
    }

    size_t size() const {
        return buffer_.size();
    }

    size_t capacity() const {
        return capacity_;
    }

private:
    struct SendWaiter {
        std::coroutine_handle<> handle;
        T value;
    };

    void wakeNextReceiver() {
        if (!recvWaiters_.empty()) {
            auto handle = recvWaiters_.front();
            recvWaiters_.pop_front();
            resumeHandle(handle);
        }
    }

    void wakeNextSender() {
        if (!sendWaiters_.empty() && buffer_.size() < capacity_) {
            auto waiter = std::move(sendWaiters_.front());
            sendWaiters_.pop_front();

            // Move the value into the buffer
            buffer_.push_back(std::move(waiter.value));

            // Wake up a receiver if any
            wakeNextReceiver();

            // Resume the sender
            resumeHandle(waiter.handle);
        }
    }

    void resumeHandle(std::coroutine_handle<> handle) {
        auto loop = EventLoop::current();
        if (loop) {
            loop->post([handle]() { handle.resume(); });
        } else {
            handle.resume();
        }
    }

    size_t capacity_;
    std::atomic<bool> closed_;
    std::deque<T> buffer_;
    std::deque<std::coroutine_handle<>> recvWaiters_;
    std::deque<SendWaiter> sendWaiters_;
};

/// @brief Sending end of a channel
///
/// Can be cloned to create multiple senders (MPMC).
/// When all senders are dropped, the channel is automatically closed.
template<typename T>
class Sender {
public:
    Sender(std::shared_ptr<ChannelImpl<T>> impl) : impl_(impl) {}

    /// Send a value through the channel
    /// Suspends if the channel is full until space is available
    /// Returns false if the channel is closed
    Task<bool> send(T value) {
        if (!impl_ || impl_->isClosed()) {
            co_return false;
        }

        // Try non-blocking send first
        if (impl_->trySend(std::move(value))) {
            co_return true;
        }

        // Need to wait
        auto awaiter = impl_->sendAwaiter(std::move(value));
        co_return co_await awaiter;
    }

    /// Try to send without blocking
    /// Returns false if channel is full or closed
    bool trySend(T value) {
        if (!impl_) return false;
        return impl_->trySend(std::move(value));
    }

    /// Close the channel
    /// All pending sends will fail, and receivers will get None after buffer is drained
    void close() {
        if (impl_) {
            impl_->close();
        }
    }

    /// Check if channel is closed
    bool isClosed() const {
        return !impl_ || impl_->isClosed();
    }

    /// Get number of items currently in channel
    size_t size() const {
        return impl_ ? impl_->size() : 0;
    }

    /// Get channel capacity
    size_t capacity() const {
        return impl_ ? impl_->capacity() : 0;
    }

private:
    std::shared_ptr<ChannelImpl<T>> impl_;
};

/// @brief Receiving end of a channel
///
/// Can be cloned to create multiple receivers (MPMC).
template<typename T>
class Receiver {
public:
    Receiver(std::shared_ptr<ChannelImpl<T>> impl) : impl_(impl) {}

    /// Receive a value from the channel
    /// Suspends if the channel is empty until data is available
    /// Returns None if the channel is closed and empty
    Task<std::optional<T>> recv() {
        if (!impl_) {
            co_return std::nullopt;
        }

        // Try non-blocking receive first
        auto value = impl_->tryRecv();
        if (value) {
            co_return value;
        }

        // Channel is empty, check if closed
        if (impl_->isClosed()) {
            co_return std::nullopt;
        }

        // Need to wait
        auto awaiter = impl_->recvAwaiter();
        co_return co_await awaiter;
    }

    /// Try to receive without blocking
    /// Returns None if channel is empty
    std::optional<T> tryRecv() {
        if (!impl_) return std::nullopt;
        return impl_->tryRecv();
    }

    /// Check if channel is closed
    bool isClosed() const {
        return !impl_ || impl_->isClosed();
    }

    /// Get number of items currently in channel
    size_t size() const {
        return impl_ ? impl_->size() : 0;
    }

    /// Get channel capacity
    size_t capacity() const {
        return impl_ ? impl_->capacity() : 0;
    }

private:
    std::shared_ptr<ChannelImpl<T>> impl_;
};

/// @brief Create a bounded MPMC channel
///
/// Creates a channel with the specified capacity. When the channel is full,
/// senders will block. When the channel is empty, receivers will block.
///
/// Example:
/// @code
/// auto [tx, rx] = channel<int>(10);
///
/// auto sender = [tx]() -> Task<void> {
///     for (int i = 0; i < 100; i++) {
///         co_await tx.send(i);
///     }
///     tx.close();
/// };
///
/// auto receiver = [rx]() -> Task<void> {
///     while (auto value = co_await rx.recv()) {
///         std::cout << "Received: " << *value << "\n";
///     }
/// };
/// @endcode
///
/// @param capacity Maximum number of items the channel can hold
/// @return Pair of (Sender, Receiver)
template<typename T>
std::pair<Sender<T>, Receiver<T>> channel(size_t capacity) {
    auto impl = std::make_shared<ChannelImpl<T>>(capacity);
    return {Sender<T>(impl), Receiver<T>(impl)};
}

} // namespace pman::async
