#pragma once
#include "pman/async/task.hpp"
#include <optional>
#include <utility>
#include <memory>

namespace pman::async {

template<typename T> class SenderImpl;
template<typename T> class ReceiverImpl;
template<typename T> class Sender;
template<typename T> class Receiver;

template<typename T>
std::pair<Sender<T>, Receiver<T>> channel(size_t capacity);

template<typename T>
class Sender {
public:
    Sender(std::shared_ptr<SenderImpl<T>> impl) : impl_(impl) {}
    Task<void> send(T value);
    void close();

private:
    std::shared_ptr<SenderImpl<T>> impl_;
};

template<typename T>
class Receiver {
public:
    Receiver(std::shared_ptr<ReceiverImpl<T>> impl) : impl_(impl) {}
    Task<std::optional<T>> recv();

private:
    std::shared_ptr<ReceiverImpl<T>> impl_;
};

} // namespace pman::async
