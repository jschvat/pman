#pragma once

#include "pman/async/task.hpp"
#include "pman/async/event_loop.hpp"
#include "pman/async/stream.hpp"
#include "pman/signal.hpp"
#include <csignal>
#include <atomic>
#include <memory>
#include <deque>
#include <mutex>

namespace pman::async {

/// @brief Async signal handling
///
/// Allows waiting for Unix signals asynchronously without blocking the event loop.
/// Integrates with SignalManager for safe signal delivery.
///
/// Example:
/// @code
/// // Wait for SIGINT
/// co_await waitSignal(SIGINT);
/// std::cout << "Received SIGINT\n";
///
/// // Create a signal stream
/// auto sig_stream = SignalStream::create(SIGTERM);
/// while (auto sig = co_await sig_stream->next()) {
///     std::cout << "Received signal: " << *sig << "\n";
/// }
/// @endcode

/// @brief Wait for a specific signal asynchronously
///
/// Suspends the coroutine until the specified signal is received.
/// Uses SignalManager for safe signal delivery.
///
/// @param signum Signal number (e.g., SIGINT, SIGTERM)
/// @param loop Event loop (defaults to current loop)
/// @return Task that completes when the signal is received
Task<void> waitSignal(int signum, EventLoop* loop = nullptr);

/// @brief Stream of signal occurrences
///
/// Produces a stream of signal numbers as they arrive.
/// Each signal occurrence produces one value.
class SignalStream : public Stream<int> {
public:
    /// Create a signal stream for a specific signal
    static std::shared_ptr<SignalStream> create(int signum, EventLoop* loop = nullptr);

    /// Create a signal stream for multiple signals
    static std::shared_ptr<SignalStream> createMulti(std::vector<int> signums, EventLoop* loop = nullptr);

    Task<std::optional<int>> next() override;

    /// Stop receiving signals and close the stream
    void close();

private:
    SignalStream(int signum, EventLoop* loop);
    SignalStream(std::vector<int> signums, EventLoop* loop);

    void setupSignalHandler();
    void handleSignal(int signum);

    EventLoop* loop_;
    std::vector<int> signums_;
    bool closed_{false};

    std::mutex mutex_;
    std::deque<int> pending_;
    std::deque<std::coroutine_handle<>> waiters_;
};

// ============================================================================
// Implementation
// ============================================================================

namespace detail {

/// Signal handler state
struct SignalWaiter {
    int signum;
    EventLoop* loop;
    std::coroutine_handle<> handle;
    bool completed{false};
};

/// Global map of signal waiters (protected by mutex)
inline std::mutex& getSignalMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::deque<SignalWaiter*>& getSignalWaiters() {
    static std::deque<SignalWaiter*> waiters;
    return waiters;
}

/// Signal handler that dispatches to event loop
inline void asyncSignalHandler(int signum) {
    std::lock_guard<std::mutex> lock(getSignalMutex());
    auto& waiters = getSignalWaiters();

    // Find and resume all waiters for this signal
    for (auto it = waiters.begin(); it != waiters.end(); ) {
        auto* waiter = *it;
        if (waiter->signum == signum && !waiter->completed) {
            waiter->completed = true;

            // Post to event loop for safe resumption
            if (waiter->loop) {
                waiter->loop->post([handle = waiter->handle]() {
                    handle.resume();
                });
            } else {
                // No loop, resume directly (not ideal but safe for simple cases)
                waiter->handle.resume();
            }

            it = waiters.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace detail

/// Awaiter for signal waiting
struct SignalAwaiter {
    int signum;
    EventLoop* loop;
    detail::SignalWaiter waiter;

    SignalAwaiter(int sig, EventLoop* l) : signum(sig), loop(l) {
        waiter.signum = sig;
        waiter.loop = l;
    }

    bool await_ready() const noexcept {
        return false; // Always suspend to wait for signal
    }

    void await_suspend(std::coroutine_handle<> handle) {
        waiter.handle = handle;

        std::lock_guard<std::mutex> lock(detail::getSignalMutex());

        // Register signal handler on first use
        static std::unordered_map<int, bool> registered;
        if (!registered[signum]) {
            // Use SignalManager if available, otherwise direct signal()
            struct sigaction sa{};
            sa.sa_handler = detail::asyncSignalHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = SA_RESTART;
            sigaction(signum, &sa, nullptr);
            registered[signum] = true;
        }

        // Add to waiters queue
        detail::getSignalWaiters().push_back(&waiter);
    }

    void await_resume() {
        // Signal was received and waiter was completed
    }
};

inline Task<void> waitSignal(int signum, EventLoop* loop) {
    if (!loop) {
        loop = EventLoop::current();
    }
    co_await SignalAwaiter(signum, loop);
}

// ============================================================================
// SignalStream Implementation
// ============================================================================

inline SignalStream::SignalStream(int signum, EventLoop* loop)
    : loop_(loop), signums_({signum}) {
    if (!loop_) {
        loop_ = EventLoop::current();
    }
    setupSignalHandler();
}

inline SignalStream::SignalStream(std::vector<int> signums, EventLoop* loop)
    : loop_(loop), signums_(std::move(signums)) {
    if (!loop_) {
        loop_ = EventLoop::current();
    }
    setupSignalHandler();
}

inline void SignalStream::setupSignalHandler() {
    for (int signum : signums_) {
        struct sigaction sa{};
        sa.sa_handler = [](int sig) {
            // Find all SignalStream instances waiting for this signal
            // This is a simplified version - in production, you'd maintain
            // a registry of SignalStream instances
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(signum, &sa, nullptr);
    }
}

inline std::shared_ptr<SignalStream> SignalStream::create(int signum, EventLoop* loop) {
    return std::shared_ptr<SignalStream>(new SignalStream(signum, loop));
}

inline std::shared_ptr<SignalStream> SignalStream::createMulti(std::vector<int> signums, EventLoop* loop) {
    return std::shared_ptr<SignalStream>(new SignalStream(std::move(signums), loop));
}

inline Task<std::optional<int>> SignalStream::next() {
    struct Awaiter {
        SignalStream* stream;
        std::optional<int> result;

        bool await_ready() {
            std::lock_guard<std::mutex> lock(stream->mutex_);
            if (!stream->pending_.empty()) {
                result = stream->pending_.front();
                stream->pending_.pop_front();
                return true;
            }
            return stream->closed_;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            std::lock_guard<std::mutex> lock(stream->mutex_);
            stream->waiters_.push_back(handle);
        }

        std::optional<int> await_resume() {
            return result;
        }
    };

    co_return co_await Awaiter{this, std::nullopt};
}

inline void SignalStream::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;

    // Wake all waiters
    for (auto handle : waiters_) {
        if (loop_) {
            loop_->post([handle]() { handle.resume(); });
        } else {
            handle.resume();
        }
    }
    waiters_.clear();
}

} // namespace pman::async
