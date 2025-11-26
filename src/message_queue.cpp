#include "pman/message_queue.hpp"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <mqueue.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace pman {

namespace {

void shared_futex_wait(std::atomic<int>* addr, int expected) {
    syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAIT,
        expected,
        nullptr,
        nullptr,
        0);
}

void shared_futex_wake(std::atomic<int>* addr, int count) {
    syscall(SYS_futex,
        reinterpret_cast<int*>(addr),
        FUTEX_WAKE,
        count,
        nullptr,
        nullptr,
        0);
}

}  // namespace

PosixMessageQueue::PosixMessageQueue(std::string name, SharedMemoryMode mode, Options options)
    : name_(std::move(name)), maxSize_(options.maxMessageSize) {

    int flags = 0;

    switch (mode) {
    case SharedMemoryMode::Create:
        flags = O_CREAT | O_EXCL | O_RDWR;
        break;
    case SharedMemoryMode::Open:
        flags = O_RDWR;
        break;
    case SharedMemoryMode::OpenOrCreate:
        flags = O_CREAT | O_RDWR;
        break;
    }

    if (!options.blocking) {
        flags |= O_NONBLOCK;
    }

    struct mq_attr attr{};
    attr.mq_maxmsg = options.maxMessages;
    attr.mq_msgsize = static_cast<long>(options.maxMessageSize);

    mqd_ = mq_open(name_.c_str(), flags, options.permissions, &attr);
    if (mqd_ == -1) {
        throw std::system_error(errno, std::generic_category(), "mq_open");
    }
}

PosixMessageQueue::~PosixMessageQueue() {
    if (mqd_ != -1) {
        mq_close(mqd_);
    }
}

PosixMessageQueue::PosixMessageQueue(PosixMessageQueue&& other) noexcept
    : name_(std::move(other.name_)),
      mqd_(other.mqd_),
      maxSize_(other.maxSize_) {
    other.mqd_ = -1;
}

PosixMessageQueue& PosixMessageQueue::operator=(PosixMessageQueue&& other) noexcept {
    if (this != &other) {
        if (mqd_ != -1) {
            mq_close(mqd_);
        }
        name_ = std::move(other.name_);
        mqd_ = other.mqd_;
        maxSize_ = other.maxSize_;
        other.mqd_ = -1;
    }
    return *this;
}

void PosixMessageQueue::send(std::span<const std::byte> data, unsigned int priority) {
    sendImpl(data, priority, nullptr);
}

bool PosixMessageQueue::trySend(std::span<const std::byte> data, unsigned int priority) {
    timespec ts{0, 0};
    return sendImpl(data, priority, &ts);
}

bool PosixMessageQueue::sendImpl(std::span<const std::byte> data, unsigned int priority,
                                  const timespec* timeout) {
    int rc;
    if (timeout) {
        rc = mq_timedsend(mqd_, reinterpret_cast<const char*>(data.data()),
                          data.size(), priority, timeout);
    } else {
        rc = mq_send(mqd_, reinterpret_cast<const char*>(data.data()),
                     data.size(), priority);
    }

    if (rc == -1) {
        if (errno == EAGAIN || errno == ETIMEDOUT) {
            return false;
        }
        throw std::system_error(errno, std::generic_category(), "mq_send");
    }
    return true;
}

std::size_t PosixMessageQueue::receive(std::span<std::byte> buffer, unsigned int* priority) {
    auto result = receiveImpl(buffer, priority, nullptr);
    if (!result) {
        throw std::runtime_error("unexpected receive failure");
    }
    return *result;
}

std::optional<std::size_t> PosixMessageQueue::tryReceive(std::span<std::byte> buffer,
                                                          unsigned int* priority) {
    timespec ts{0, 0};
    return receiveImpl(buffer, priority, &ts);
}

std::optional<std::size_t> PosixMessageQueue::receiveImpl(std::span<std::byte> buffer,
                                                           unsigned int* priority,
                                                           const timespec* timeout) {
    ssize_t rc;
    if (timeout) {
        rc = mq_timedreceive(mqd_, reinterpret_cast<char*>(buffer.data()),
                             buffer.size(), priority, timeout);
    } else {
        rc = mq_receive(mqd_, reinterpret_cast<char*>(buffer.data()),
                        buffer.size(), priority);
    }

    if (rc == -1) {
        if (errno == EAGAIN || errno == ETIMEDOUT) {
            return std::nullopt;
        }
        throw std::system_error(errno, std::generic_category(), "mq_receive");
    }
    return static_cast<std::size_t>(rc);
}

long PosixMessageQueue::currentMessages() const {
    struct mq_attr attr;
    if (mq_getattr(mqd_, &attr) == -1) {
        throw std::system_error(errno, std::generic_category(), "mq_getattr");
    }
    return attr.mq_curmsgs;
}

void PosixMessageQueue::unlink(const std::string& name) {
    if (mq_unlink(name.c_str()) == -1 && errno != ENOENT) {
        throw std::system_error(errno, std::generic_category(), "mq_unlink");
    }
}

}  // namespace pman
