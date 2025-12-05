#pragma once

#include "pman/async/task.hpp"
#include "pman/async/channel.hpp"
#include <optional>
#include <functional>
#include <memory>
#include <vector>

namespace pman::async {

/// @brief Async stream abstraction for processing sequences of values
///
/// Streams are lazy, composable sequences that produce values asynchronously.
/// Similar to Rust's Stream trait or JavaScript's AsyncIterator.
///
/// Example:
/// @code
/// auto stream = Stream<int>::fromChannel(rx)
///     .map([](int x) { return x * 2; })
///     .filter([](int x) { return x > 10; })
///     .take(5);
///
/// while (auto value = co_await stream.next()) {
///     std::cout << *value << "\n";
/// }
/// @endcode
template<typename T>
class Stream {
public:
    virtual ~Stream() = default;

    /// Get the next value from the stream
    /// Returns std::nullopt when the stream is exhausted
    virtual Task<std::optional<T>> next() = 0;

    /// Collect all remaining values into a vector
    Task<std::vector<T>> collect() {
        std::vector<T> result;
        while (auto value = co_await next()) {
            result.push_back(std::move(*value));
        }
        co_return result;
    }

    /// Count remaining elements
    Task<size_t> count() {
        size_t n = 0;
        while (auto value = co_await next()) {
            (void)value;
            n++;
        }
        co_return n;
    }

    /// Create a stream from a channel receiver
    static std::shared_ptr<Stream<T>> fromChannel(Receiver<T> rx);

    /// Create a stream from a vector
    static std::shared_ptr<Stream<T>> fromVector(std::vector<T> values);

    /// Transform each element
    template<typename F, typename U = std::invoke_result_t<F, T>>
    std::shared_ptr<Stream<U>> map(F func);

    /// Filter elements by predicate
    template<typename F>
    std::shared_ptr<Stream<T>> filter(F pred);

    /// Take first n elements
    std::shared_ptr<Stream<T>> take(size_t n);

    /// Skip first n elements
    std::shared_ptr<Stream<T>> skip(size_t n);

    /// Take elements while predicate is true
    template<typename F>
    std::shared_ptr<Stream<T>> takeWhile(F pred);

    /// Chain two streams together
    std::shared_ptr<Stream<T>> chain(std::shared_ptr<Stream<T>> other);

    /// Execute a function for each element
    template<typename F>
    Task<void> forEach(F func) {
        while (auto value = co_await next()) {
            func(*value);
        }
    }
};

// ============================================================================
// Implementation: ChannelStream
// ============================================================================

template<typename T>
class ChannelStream : public Stream<T> {
public:
    explicit ChannelStream(Receiver<T> rx) : rx_(std::move(rx)) {}

    Task<std::optional<T>> next() override {
        co_return co_await rx_.recv();
    }

private:
    Receiver<T> rx_;
};

template<typename T>
std::shared_ptr<Stream<T>> Stream<T>::fromChannel(Receiver<T> rx) {
    return std::make_shared<ChannelStream<T>>(std::move(rx));
}

// ============================================================================
// Implementation: VectorStream
// ============================================================================

template<typename T>
class VectorStream : public Stream<T> {
public:
    explicit VectorStream(std::vector<T> values)
        : values_(std::move(values)), index_(0) {}

    Task<std::optional<T>> next() override {
        if (index_ < values_.size()) {
            co_return std::move(values_[index_++]);
        }
        co_return std::nullopt;
    }

private:
    std::vector<T> values_;
    size_t index_;
};

template<typename T>
std::shared_ptr<Stream<T>> Stream<T>::fromVector(std::vector<T> values) {
    return std::make_shared<VectorStream<T>>(std::move(values));
}

// ============================================================================
// Implementation: MapStream
// ============================================================================

template<typename T, typename U, typename F>
class MapStream : public Stream<U> {
public:
    MapStream(std::shared_ptr<Stream<T>> source, F func)
        : source_(std::move(source)), func_(std::move(func)) {}

    Task<std::optional<U>> next() override {
        auto value = co_await source_->next();
        if (!value) {
            co_return std::nullopt;
        }
        co_return func_(std::move(*value));
    }

private:
    std::shared_ptr<Stream<T>> source_;
    F func_;
};

template<typename T>
template<typename F, typename U>
std::shared_ptr<Stream<U>> Stream<T>::map(F func) {
    return std::make_shared<MapStream<T, U, F>>(
        std::shared_ptr<Stream<T>>(this, [](Stream<T>*) {}), // Non-owning shared_ptr
        std::move(func)
    );
}

// ============================================================================
// Implementation: FilterStream
// ============================================================================

template<typename T, typename F>
class FilterStream : public Stream<T> {
public:
    FilterStream(std::shared_ptr<Stream<T>> source, F pred)
        : source_(std::move(source)), pred_(std::move(pred)) {}

    Task<std::optional<T>> next() override {
        while (true) {
            auto value = co_await source_->next();
            if (!value) {
                co_return std::nullopt;
            }
            if (pred_(*value)) {
                co_return value;
            }
            // Continue to next value if predicate fails
        }
    }

private:
    std::shared_ptr<Stream<T>> source_;
    F pred_;
};

template<typename T>
template<typename F>
std::shared_ptr<Stream<T>> Stream<T>::filter(F pred) {
    return std::make_shared<FilterStream<T, F>>(
        std::shared_ptr<Stream<T>>(this, [](Stream<T>*) {}),
        std::move(pred)
    );
}

// ============================================================================
// Implementation: TakeStream
// ============================================================================

template<typename T>
class TakeStream : public Stream<T> {
public:
    TakeStream(std::shared_ptr<Stream<T>> source, size_t n)
        : source_(std::move(source)), n_(n), count_(0) {}

    Task<std::optional<T>> next() override {
        if (count_ >= n_) {
            co_return std::nullopt;
        }
        count_++;
        co_return co_await source_->next();
    }

private:
    std::shared_ptr<Stream<T>> source_;
    size_t n_;
    size_t count_;
};

template<typename T>
std::shared_ptr<Stream<T>> Stream<T>::take(size_t n) {
    return std::make_shared<TakeStream<T>>(
        std::shared_ptr<Stream<T>>(this, [](Stream<T>*) {}),
        n
    );
}

// ============================================================================
// Implementation: SkipStream
// ============================================================================

template<typename T>
class SkipStream : public Stream<T> {
public:
    SkipStream(std::shared_ptr<Stream<T>> source, size_t n)
        : source_(std::move(source)), n_(n), skipped_(0) {}

    Task<std::optional<T>> next() override {
        while (skipped_ < n_) {
            auto value = co_await source_->next();
            if (!value) {
                co_return std::nullopt;
            }
            skipped_++;
        }
        co_return co_await source_->next();
    }

private:
    std::shared_ptr<Stream<T>> source_;
    size_t n_;
    size_t skipped_;
};

template<typename T>
std::shared_ptr<Stream<T>> Stream<T>::skip(size_t n) {
    return std::make_shared<SkipStream<T>>(
        std::shared_ptr<Stream<T>>(this, [](Stream<T>*) {}),
        n
    );
}

// ============================================================================
// Implementation: TakeWhileStream
// ============================================================================

template<typename T, typename F>
class TakeWhileStream : public Stream<T> {
public:
    TakeWhileStream(std::shared_ptr<Stream<T>> source, F pred)
        : source_(std::move(source)), pred_(std::move(pred)), done_(false) {}

    Task<std::optional<T>> next() override {
        if (done_) {
            co_return std::nullopt;
        }
        auto value = co_await source_->next();
        if (!value || !pred_(*value)) {
            done_ = true;
            co_return std::nullopt;
        }
        co_return value;
    }

private:
    std::shared_ptr<Stream<T>> source_;
    F pred_;
    bool done_;
};

template<typename T>
template<typename F>
std::shared_ptr<Stream<T>> Stream<T>::takeWhile(F pred) {
    return std::make_shared<TakeWhileStream<T, F>>(
        std::shared_ptr<Stream<T>>(this, [](Stream<T>*) {}),
        std::move(pred)
    );
}

// ============================================================================
// Implementation: ChainStream
// ============================================================================

template<typename T>
class ChainStream : public Stream<T> {
public:
    ChainStream(std::shared_ptr<Stream<T>> first, std::shared_ptr<Stream<T>> second)
        : first_(std::move(first)), second_(std::move(second)), on_second_(false) {}

    Task<std::optional<T>> next() override {
        if (!on_second_) {
            auto value = co_await first_->next();
            if (value) {
                co_return value;
            }
            on_second_ = true;
        }
        co_return co_await second_->next();
    }

private:
    std::shared_ptr<Stream<T>> first_;
    std::shared_ptr<Stream<T>> second_;
    bool on_second_;
};

template<typename T>
std::shared_ptr<Stream<T>> Stream<T>::chain(std::shared_ptr<Stream<T>> other) {
    return std::make_shared<ChainStream<T>>(
        std::shared_ptr<Stream<T>>(this, [](Stream<T>*) {}),
        std::move(other)
    );
}

} // namespace pman::async
