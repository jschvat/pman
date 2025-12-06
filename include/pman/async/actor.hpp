#pragma once

#include "pman/async/task.hpp"
#include "pman/async/channel.hpp"
#include "pman/async/event_loop.hpp"
#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <variant>
#include <optional>
#include <atomic>

namespace pman::async {

/// @brief Unique identifier for an actor/process
using ProcessId = uint64_t;

/// @brief Special process ID values
constexpr ProcessId INVALID_PID = 0;

/// @brief Message types for actor communication
/// Users can extend this with their own message types
struct Message {
    /// Source process ID
    ProcessId from;

    /// Message payload (use std::variant or std::any for custom types)
    std::shared_ptr<void> payload;

    /// Type-safe payload access
    template<typename T>
    T* as() {
        return static_cast<T*>(payload.get());
    }

    template<typename T>
    const T* as() const {
        return static_cast<const T*>(payload.get());
    }
};

/// @brief Process exit reason
enum class ExitReason {
    Normal,      // Clean exit
    Killed,      // Explicitly killed
    Exception,   // Unhandled exception
    Linked       // Linked process died
};

/// @brief Exit signal sent when a process terminates
struct ExitSignal {
    ProcessId pid;
    ExitReason reason;
    std::string message;
};

/// @brief Monitor notification
struct DownMessage {
    ProcessId pid;       // The monitored process
    ProcessId monitor;   // The monitor reference
    ExitReason reason;
};

// Forward declarations
class Actor;
class ProcessRegistry;

/// @brief Actor context - provides access to self PID, registry, etc.
struct ActorContext {
    ProcessId self_pid;
    ProcessRegistry* registry;
    EventLoop* loop;

    /// Send a message to another process
    Task<bool> send(ProcessId to, std::shared_ptr<void> payload);

    /// Link to another process (bidirectional crash propagation)
    Task<void> link(ProcessId other);

    /// Monitor another process (one-way death notification)
    Task<ProcessId> monitor(ProcessId other);

    /// Exit the current process
    [[noreturn]] void exit(ExitReason reason = ExitReason::Normal, const std::string& msg = "");
};

/// @brief Base class for actors
///
/// Actors run concurrently, communicate via message passing,
/// and can be supervised for fault tolerance.
///
/// Example:
/// @code
/// class MyActor : public Actor {
/// protected:
///     Task<void> run(ActorContext& ctx) override {
///         while (true) {
///             auto msg = co_await receive();
///             if (!msg) break;  // Mailbox closed
///
///             // Process message
///             if (auto* data = msg->as<int>()) {
///                 std::cout << "Received: " << *data << "\n";
///             }
///         }
///     }
/// };
///
/// // Spawn the actor
/// auto pid = spawn<MyActor>();
///
/// // Send a message
/// send(pid, std::make_shared<int>(42));
/// @endcode
class Actor {
public:
    Actor();
    virtual ~Actor();

    /// Get this actor's process ID
    ProcessId pid() const { return pid_; }

    /// Check if actor is alive
    bool isAlive() const { return !stopped_.load(std::memory_order_acquire); }

    /// Get actor context (for advanced use)
    ActorContext& getContext() { return context_; }

    /// Receive the next message from mailbox
    /// Returns nullopt when mailbox is closed
    Task<std::optional<Message>> receive();

    /// Send a message to this actor's mailbox
    Task<bool> sendMessage(Message msg);

    /// Stop the actor gracefully
    void stop();

protected:
    /// Main actor loop - override this to implement actor behavior
    virtual Task<void> run(ActorContext& ctx) = 0;

    /// Called when actor receives an exit signal from a linked process
    /// Default behavior: exit with same reason
    /// Override to handle differently (e.g., trap exits)
    virtual Task<void> handleExitSignal(const ExitSignal& signal);

    /// Internal: Start the actor (called by spawn())
    void start(ProcessId pid, ProcessRegistry* registry, EventLoop* loop);

    /// Internal: Handle actor crash
    void handleCrash(ExitReason reason, const std::string& msg);

private:
    friend class ProcessRegistry;

    ProcessId pid_{INVALID_PID};
    ProcessRegistry* registry_{nullptr};
    EventLoop* loop_{nullptr};

    std::shared_ptr<ChannelImpl<Message>> mailbox_impl_;
    Sender<Message> mailbox_tx_;
    Receiver<Message> mailbox_rx_;

    std::atomic<bool> stopped_{false};
    std::atomic<bool> started_{false};

    // Links (bidirectional crash propagation)
    std::mutex links_mutex_;
    std::unordered_set<ProcessId> links_;

    // Monitors watching this process
    std::mutex monitors_mutex_;
    std::unordered_set<ProcessId> monitors_;

    ActorContext context_;
};

/// @brief Process registry for name-based process lookup
///
/// Allows registering processes with string names for easier communication.
/// Similar to Erlang's process registry.
class ProcessRegistry {
public:
    ProcessRegistry(EventLoop* loop) : loop_(loop) {}

    /// Spawn a new actor
    template<typename T, typename... Args>
    ProcessId spawn(Args&&... args);

    /// Register a process with a name
    /// Returns false if name is already taken
    bool registerName(const std::string& name, ProcessId pid);

    /// Unregister a name
    void unregisterName(const std::string& name);

    /// Lookup a process by name
    std::optional<ProcessId> whereis(const std::string& name);

    /// Send a message to a process by PID
    Task<bool> send(ProcessId to, ProcessId from, std::shared_ptr<void> payload);

    /// Send a message to a named process
    Task<bool> send(const std::string& name, ProcessId from, std::shared_ptr<void> payload);

    /// Link two processes (bidirectional crash propagation)
    void link(ProcessId pid1, ProcessId pid2);

    /// Monitor a process (one-way notification on death)
    /// Returns monitor reference ID
    ProcessId monitor(ProcessId watcher, ProcessId watched);

    /// Internal: Notify of process exit
    void notifyExit(ProcessId pid, ExitReason reason, const std::string& msg);

    /// Get actor by PID (internal)
    std::shared_ptr<Actor> getActor(ProcessId pid);

private:
    EventLoop* loop_;

    std::atomic<ProcessId> next_pid_{1};

    std::mutex actors_mutex_;
    std::unordered_map<ProcessId, std::shared_ptr<Actor>> actors_;

    std::mutex names_mutex_;
    std::unordered_map<std::string, ProcessId> names_;
    std::unordered_map<ProcessId, std::string> reverse_names_;
};

// ============================================================================
// Implementation
// ============================================================================

inline Actor::Actor()
    : mailbox_impl_(std::make_shared<ChannelImpl<Message>>(1000))
    , mailbox_tx_(mailbox_impl_)
    , mailbox_rx_(mailbox_impl_) {
}

inline Actor::~Actor() {
    stop();
}

inline Task<std::optional<Message>> Actor::receive() {
    co_return co_await mailbox_rx_.recv();
}

inline Task<bool> Actor::sendMessage(Message msg) {
    if (stopped_.load(std::memory_order_acquire)) {
        co_return false;
    }
    co_return co_await mailbox_tx_.send(std::move(msg));
}

inline void Actor::stop() {
    bool expected = false;
    if (stopped_.compare_exchange_strong(expected, true)) {
        mailbox_tx_.close();
    }
}

inline void Actor::start(ProcessId pid, ProcessRegistry* registry, EventLoop* loop) {
    pid_ = pid;
    registry_ = registry;
    loop_ = loop;

    context_.self_pid = pid;
    context_.registry = registry;
    context_.loop = loop;

    bool expected = false;
    if (started_.compare_exchange_strong(expected, true)) {
        auto self = this;

        auto runTask = [self]() -> Task<void> {
            try {
                co_await self->run(self->context_);
                self->handleCrash(ExitReason::Normal, "");
            } catch (const std::exception& e) {
                self->handleCrash(ExitReason::Exception, e.what());
            } catch (...) {
                self->handleCrash(ExitReason::Exception, "Unknown exception");
            }
        };

        runTask().start();
    }
}

inline void Actor::handleCrash(ExitReason reason, const std::string& msg) {
    stop();
    if (registry_) {
        registry_->notifyExit(pid_, reason, msg);
    }
}

inline Task<void> Actor::handleExitSignal(const ExitSignal& signal) {
    // Default behavior: propagate the exit
    handleCrash(ExitReason::Linked, "Linked process " + std::to_string(signal.pid) + " died");
    co_return;
}

// ============================================================================
// ProcessRegistry Implementation
// ============================================================================

template<typename T, typename... Args>
ProcessId ProcessRegistry::spawn(Args&&... args) {
    static_assert(std::is_base_of_v<Actor, T>, "T must derive from Actor");

    ProcessId pid = next_pid_.fetch_add(1, std::memory_order_relaxed);
    auto actor = std::make_shared<T>(std::forward<Args>(args)...);

    {
        std::lock_guard<std::mutex> lock(actors_mutex_);
        actors_[pid] = actor;
    }

    actor->start(pid, this, loop_);

    return pid;
}

inline bool ProcessRegistry::registerName(const std::string& name, ProcessId pid) {
    std::lock_guard<std::mutex> lock(names_mutex_);

    if (names_.find(name) != names_.end()) {
        return false;  // Name already taken
    }

    names_[name] = pid;
    reverse_names_[pid] = name;
    return true;
}

inline void ProcessRegistry::unregisterName(const std::string& name) {
    std::lock_guard<std::mutex> lock(names_mutex_);

    auto it = names_.find(name);
    if (it != names_.end()) {
        reverse_names_.erase(it->second);
        names_.erase(it);
    }
}

inline std::optional<ProcessId> ProcessRegistry::whereis(const std::string& name) {
    std::lock_guard<std::mutex> lock(names_mutex_);

    auto it = names_.find(name);
    if (it != names_.end()) {
        return it->second;
    }
    return std::nullopt;
}

inline Task<bool> ProcessRegistry::send(ProcessId to, ProcessId from, std::shared_ptr<void> payload) {
    std::shared_ptr<Actor> actor;
    {
        std::lock_guard<std::mutex> lock(actors_mutex_);
        auto it = actors_.find(to);
        if (it == actors_.end()) {
            co_return false;
        }
        actor = it->second;
    }

    Message msg{from, std::move(payload)};
    co_return co_await actor->sendMessage(std::move(msg));
}

inline Task<bool> ProcessRegistry::send(const std::string& name, ProcessId from, std::shared_ptr<void> payload) {
    auto pid = whereis(name);
    if (!pid) {
        co_return false;
    }
    co_return co_await send(*pid, from, std::move(payload));
}

inline void ProcessRegistry::link(ProcessId pid1, ProcessId pid2) {
    auto actor1 = getActor(pid1);
    auto actor2 = getActor(pid2);

    if (!actor1 || !actor2) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock1(actor1->links_mutex_);
        actor1->links_.insert(pid2);
    }

    {
        std::lock_guard<std::mutex> lock2(actor2->links_mutex_);
        actor2->links_.insert(pid1);
    }
}

inline ProcessId ProcessRegistry::monitor(ProcessId watcher, ProcessId watched) {
    auto actor = getActor(watched);
    if (!actor) {
        return INVALID_PID;
    }

    std::lock_guard<std::mutex> lock(actor->monitors_mutex_);
    actor->monitors_.insert(watcher);

    return watcher;  // Use watcher PID as monitor reference
}

inline void ProcessRegistry::notifyExit(ProcessId pid, ExitReason reason, const std::string& msg) {
    std::shared_ptr<Actor> actor;

    {
        std::lock_guard<std::mutex> lock(actors_mutex_);
        auto it = actors_.find(pid);
        if (it != actors_.end()) {
            actor = it->second;
            actors_.erase(it);
        }
    }

    // Unregister name if any
    {
        std::lock_guard<std::mutex> lock(names_mutex_);
        auto it = reverse_names_.find(pid);
        if (it != reverse_names_.end()) {
            names_.erase(it->second);
            reverse_names_.erase(it);
        }
    }

    if (!actor) {
        return;
    }

    // Notify linked processes
    std::unordered_set<ProcessId> links;
    {
        std::lock_guard<std::mutex> lock(actor->links_mutex_);
        links = actor->links_;
    }

    ExitSignal signal{pid, reason, msg};
    for (ProcessId linked_pid : links) {
        auto linked = getActor(linked_pid);
        if (linked) {
            // Send exit signal as a special message
            auto exitTask = [linked, signal]() -> Task<void> {
                co_await linked->handleExitSignal(signal);
            };
            exitTask().start();
        }
    }

    // Notify monitors
    std::unordered_set<ProcessId> monitors;
    {
        std::lock_guard<std::mutex> lock(actor->monitors_mutex_);
        monitors = actor->monitors_;
    }

    for (ProcessId monitor_pid : monitors) {
        auto monitor = getActor(monitor_pid);
        if (monitor) {
            // Send DOWN message
            auto down = std::make_shared<DownMessage>();
            down->pid = pid;
            down->monitor = monitor_pid;
            down->reason = reason;

            auto sendTask = send(monitor_pid, INVALID_PID, down);
            sendTask.start();
        }
    }
}

inline std::shared_ptr<Actor> ProcessRegistry::getActor(ProcessId pid) {
    std::lock_guard<std::mutex> lock(actors_mutex_);
    auto it = actors_.find(pid);
    if (it != actors_.end()) {
        return it->second;
    }
    return nullptr;
}

// ============================================================================
// ActorContext Implementation
// ============================================================================

inline Task<bool> ActorContext::send(ProcessId to, std::shared_ptr<void> payload) {
    if (!registry) {
        co_return false;
    }
    co_return co_await registry->send(to, self_pid, std::move(payload));
}

inline Task<void> ActorContext::link(ProcessId other) {
    if (registry) {
        registry->link(self_pid, other);
    }
    co_return;
}

inline Task<ProcessId> ActorContext::monitor(ProcessId other) {
    if (!registry) {
        co_return INVALID_PID;
    }
    co_return registry->monitor(self_pid, other);
}

inline void ActorContext::exit(ExitReason reason, const std::string& msg) {
    if (registry) {
        registry->notifyExit(self_pid, reason, msg);
    }
    throw std::runtime_error("Actor exited: " + msg);
}

} // namespace pman::async
