#pragma once

#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"
#include <optional>
#include <utility>
#include <memory>
#include <deque>
#include <coroutine>
#include <atomic>
#include <mutex>

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
        std::shared_ptr<bool> sent_flag;

        bool await_ready() const noexcept {
            // Check if we can send immediately
            if (channel->closed_.load(std::memory_order_acquire)) {
                return true;  // Channel closed, fail immediately
            }
            std::lock_guard<std::mutex> lock(channel->mutex_);
            return channel->buffer_.size() < channel->capacity_;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            // Add to waiting senders queue with a flag pointer
            std::lock_guard<std::mutex> lock(channel->mutex_);
            channel->sendWaiters_.push_back({handle, std::move(value), sent_flag});
        }

        bool await_resume() {
            if (channel->closed_.load(std::memory_order_acquire)) {
                return false;  // Channel was closed
            }

            // Check if the value was successfully sent
            return sent_flag && *sent_flag;
        }
    };

    /// Receive a value from the channel
    /// Blocks if channel is empty until data is available or channel is closed
    struct RecvAwaiter {
        ChannelImpl<T>* channel;
        std::optional<T> result;

        bool await_ready() const noexcept {
            // Check if we can receive immediately
            if (channel->closed_.load(std::memory_order_acquire)) {
                return true;
            }
            std::lock_guard<std::mutex> lock(channel->mutex_);
            return !channel->buffer_.empty();
        }

        void await_suspend(std::coroutine_handle<> handle) {
            // Add to waiting receivers queue
            std::lock_guard<std::mutex> lock(channel->mutex_);
            channel->recvWaiters_.push_back(handle);
        }

        std::optional<T> await_resume() {
            std::lock_guard<std::mutex> lock(channel->mutex_);
            if (!channel->buffer_.empty()) {
                T value = std::move(channel->buffer_.front());
                channel->buffer_.pop_front();

                // Wake up a waiting sender if any (already holding lock)
                channel->wakeNextSenderLocked();

                return value;
            }

            // Channel closed and empty
            return std::nullopt;
        }
    };

    SendAwaiter sendAwaiter(T value) {
        return SendAwaiter{this, std::move(value), std::make_shared<bool>(false)};
    }

    RecvAwaiter recvAwaiter() {
        return RecvAwaiter{this, std::nullopt};
    }

    /// Try to send without blocking
    bool trySend(T value) {
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer_.size() >= capacity_) {
            return false;
        }

        buffer_.push_back(std::move(value));
        wakeNextReceiverLocked();
        return true;
    }

    /// Try to receive without blocking
    std::optional<T> tryRecv() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (buffer_.empty()) {
            return std::nullopt;
        }

        T value = std::move(buffer_.front());
        buffer_.pop_front();
        wakeNextSenderLocked();
        return value;
    }

    /// Close the channel
    /// Wakes all waiting senders and receivers
    void close() {
        bool expected = false;
        if (closed_.compare_exchange_strong(expected, true, std::memory_order_release)) {
            std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size();
    }

    size_t capacity() const {
        return capacity_;
    }

private:
    struct SendWaiter {
        std::coroutine_handle<> handle;
        T value;
        std::shared_ptr<bool> sent_flag;
    };

    // Must be called with mutex_ held
    void wakeNextReceiverLocked() {
        if (!recvWaiters_.empty()) {
            auto handle = recvWaiters_.front();
            recvWaiters_.pop_front();
            resumeHandle(handle);
        }
    }

    void wakeNextReceiver() {
        std::lock_guard<std::mutex> lock(mutex_);
        wakeNextReceiverLocked();
    }

    // Must be called with mutex_ held
    void wakeNextSenderLocked() {
        if (!sendWaiters_.empty() && buffer_.size() < capacity_) {
            auto waiter = std::move(sendWaiters_.front());
            sendWaiters_.pop_front();

            // Move the value into the buffer
            buffer_.push_back(std::move(waiter.value));

            // Mark as successfully sent
            if (waiter.sent_flag) {
                *waiter.sent_flag = true;
            }

            // Wake up a receiver if any
            wakeNextReceiverLocked();

            // Resume the sender
            resumeHandle(waiter.handle);
        }
    }

    void wakeNextSender() {
        std::lock_guard<std::mutex> lock(mutex_);
        wakeNextSenderLocked();
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
    mutable std::mutex mutex_;
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
