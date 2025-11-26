#include "pman/async/channel.hpp"
#include <queue>
#include <mutex>
#include <coroutine>
#include <vector>

namespace pman::async {

template<typename T>
class ChannelImpl {
public:
    explicit ChannelImpl(size_t capacity) : capacity_(capacity) {}

    bool trySend(T value) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (closed_) {
            return false;
        }

        if (queue_.size() >= capacity_) {
            return false;
        }

        queue_.push(std::move(value));

        if (!receivers_.empty()) {
            auto receiver = receivers_.front();
            receivers_.erase(receivers_.begin());
            receiver.resume();
        }

        return true;
    }

    Task<void> send(T value) {
        struct Awaiter {
            ChannelImpl* channel;
            T value;
            std::coroutine_handle<> handle;

            bool await_ready() {
                return channel->trySend(std::move(value));
            }

            void await_suspend(std::coroutine_handle<> h) {
                handle = h;
                std::lock_guard<std::mutex> lock(channel->mutex_);
                channel->senders_.push_back({h, std::move(value)});
            }

            void await_resume() {}
        };

        co_await Awaiter{this, std::move(value)};
    }

    std::optional<T> tryRecv() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (queue_.empty()) {
            if (closed_) {
                return std::nullopt;
            }
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop();

        if (!senders_.empty()) {
            auto sender = senders_.front();
            senders_.erase(senders_.begin());
            queue_.push(std::move(sender.value));
            sender.handle.resume();
        }

        return value;
    }

    Task<std::optional<T>> recv() {
        struct Awaiter {
            ChannelImpl* channel;
            std::coroutine_handle<> handle;

            bool await_ready() {
                auto result = channel->tryRecv();
                if (result.has_value()) {
                    value_ = std::move(*result);
                    hasValue_ = true;
                    return true;
                }
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) {
                handle = h;
                std::lock_guard<std::mutex> lock(channel->mutex_);

                if (channel->closed_ && channel->queue_.empty()) {
                    hasValue_ = false;
                    h.resume();
                    return;
                }

                channel->receivers_.push_back(h);
            }

            std::optional<T> await_resume() {
                if (hasValue_) {
                    return std::move(value_);
                }

                auto result = channel->tryRecv();
                if (result.has_value()) {
                    return result;
                }

                return std::nullopt;
            }

            T value_;
            bool hasValue_{false};
        };

        co_return co_await Awaiter{this};
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;

        for (auto receiver : receivers_) {
            receiver.resume();
        }
        receivers_.clear();
    }

private:
    struct SenderState {
        std::coroutine_handle<> handle;
        T value;
    };

    size_t capacity_;
    std::queue<T> queue_;
    std::mutex mutex_;
    bool closed_{false};
    std::vector<std::coroutine_handle<>> receivers_;
    std::vector<SenderState> senders_;
};

template<typename T>
class SenderImpl {
public:
    SenderImpl(std::shared_ptr<ChannelImpl<T>> impl) : impl_(impl) {}

    Task<void> send(T value) {
        return impl_->send(std::move(value));
    }

    void close() {
        impl_->close();
    }

private:
    std::shared_ptr<ChannelImpl<T>> impl_;
};

template<typename T>
class ReceiverImpl {
public:
    ReceiverImpl(std::shared_ptr<ChannelImpl<T>> impl) : impl_(impl) {}

    Task<std::optional<T>> recv() {
        return impl_->recv();
    }

private:
    std::shared_ptr<ChannelImpl<T>> impl_;
};

template<typename T>
Task<void> Sender<T>::send(T value) {
    return impl_->send(std::move(value));
}

template<typename T>
void Sender<T>::close() {
    impl_->close();
}

template<typename T>
Task<std::optional<T>> Receiver<T>::recv() {
    return impl_->recv();
}

template<typename T>
std::pair<Sender<T>, Receiver<T>> channel(size_t capacity) {
    auto impl = std::make_shared<ChannelImpl<T>>(capacity);
    return {Sender<T>{std::make_shared<SenderImpl<T>>(impl)},
            Receiver<T>{std::make_shared<ReceiverImpl<T>>(impl)}};
}

// Explicit instantiations for common types
template class Sender<int>;
template class Receiver<int>;
template std::pair<Sender<int>, Receiver<int>> channel<int>(size_t);

template class Sender<std::string>;
template class Receiver<std::string>;
template std::pair<Sender<std::string>, Receiver<std::string>> channel<std::string>(size_t);

} // namespace pman::async
