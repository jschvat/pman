#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "pman/message_queue.hpp"
#include "pman/shared_memory.hpp"
#include "pman/shared_sync.hpp"
#include "pman/unix_socket.hpp"

using namespace std::chrono_literals;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) \
    void test_##name(); \
    struct TestRunner_##name { \
        TestRunner_##name() { \
            std::printf("  Testing %s... ", #name); \
            std::fflush(stdout); \
            try { \
                test_##name(); \
                std::printf("PASSED\n"); \
                ++testsPassed; \
            } catch (const std::exception& e) { \
                std::printf("FAILED: %s\n", e.what()); \
                ++testsFailed; \
            } catch (...) { \
                std::printf("FAILED: unknown exception\n"); \
                ++testsFailed; \
            } \
        } \
    } testRunner_##name; \
    void test_##name()

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error("Assertion failed: " #cond); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            throw std::runtime_error("Assertion failed: " #a " == " #b); \
        } \
    } while (0)

// =============================================================================
// SharedMemory Tests
// =============================================================================

TEST(shared_memory_create_and_map) {
    const char* name = "/pman_test_shm_basic";

    // Clean up any previous runs
    try { pman::SharedMemory::unlink(name); } catch (...) {}

    {
        pman::SharedMemoryOptions opts;
        opts.mode = pman::SharedMemoryMode::Create;
        pman::SharedMemory shm(name, 4096, opts);
        ASSERT(shm.data() != nullptr);
        ASSERT_EQ(shm.size(), 4096u);

        // Write some data
        std::memset(shm.data(), 0xAB, 100);
    }

    // Open existing
    {
        pman::SharedMemoryOptions opts;
        opts.mode = pman::SharedMemoryMode::Open;
        pman::SharedMemory shm(name, 4096, opts);
        auto* data = static_cast<unsigned char*>(shm.data());
        ASSERT_EQ(data[0], 0xAB);
        ASSERT_EQ(data[99], 0xAB);
    }

    pman::SharedMemory::unlink(name);
}

TEST(shared_memory_typed_access) {
    const char* name = "/pman_test_shm_typed";

    try { pman::SharedMemory::unlink(name); } catch (...) {}

    struct TestData {
        int value;
        double pi;
        char message[32];
    };

    {
        pman::SharedMemoryOptions opts;
        opts.mode = pman::SharedMemoryMode::Create;
        pman::SharedMemory shm(name, sizeof(TestData), opts);
        auto* data = shm.as<TestData>();

        data->value = 42;
        data->pi = 3.14159;
        std::strcpy(data->message, "Hello, shared memory!");
    }

    {
        pman::SharedMemoryOptions opts;
        opts.mode = pman::SharedMemoryMode::Open;
        pman::SharedMemory shm(name, sizeof(TestData), opts);
        auto* data = shm.as<TestData>();

        ASSERT_EQ(data->value, 42);
        ASSERT(data->pi > 3.14 && data->pi < 3.15);
        ASSERT(std::strcmp(data->message, "Hello, shared memory!") == 0);
    }

    pman::SharedMemory::unlink(name);
}

TEST(shared_memory_create_or_open) {
    const char* name = "/pman_test_shm_create_open";

    try { pman::SharedMemory::unlink(name); } catch (...) {}

    // OpenOrCreate mode should create if doesn't exist
    {
        pman::SharedMemoryOptions opts;
        opts.mode = pman::SharedMemoryMode::OpenOrCreate;
        pman::SharedMemory shm(name, 4096, opts);
        ASSERT(shm.data() != nullptr);
        std::memset(shm.data(), 0x55, 100);
    }

    // Should be able to open again
    {
        pman::SharedMemoryOptions opts;
        opts.mode = pman::SharedMemoryMode::Open;
        pman::SharedMemory shm(name, 4096, opts);
        auto* data = static_cast<unsigned char*>(shm.data());
        ASSERT_EQ(data[0], 0x55);
    }

    pman::SharedMemory::unlink(name);
}

// =============================================================================
// SharedSync Tests
// =============================================================================

TEST(shared_futex_mutex_basic) {
    pman::SharedFutexMutex mutex;

    mutex.lock();
    // Should be locked
    mutex.unlock();

    ASSERT(mutex.try_lock());
    mutex.unlock();
}

TEST(shared_futex_mutex_between_threads) {
    pman::SharedFutexMutex mutex;
    int value = 0;

    auto worker = [&]() {
        for (int i = 0; i < 1000; ++i) {
            mutex.lock();
            ++value;
            mutex.unlock();
        }
    };

    std::thread t1(worker), t2(worker);
    t1.join();
    t2.join();

    ASSERT_EQ(value, 2000);
}

TEST(shared_futex_condvar_signal) {
    pman::SharedFutexMutex mutex;
    pman::SharedFutexCondVar cv;
    bool ready = false;
    bool processed = false;

    std::thread worker([&]() {
        mutex.lock();
        while (!ready) {
            cv.wait(mutex);
        }
        processed = true;
        mutex.unlock();
    });

    std::this_thread::sleep_for(50ms);
    ASSERT(!processed);

    mutex.lock();
    ready = true;
    cv.notify_one();
    mutex.unlock();

    worker.join();
    ASSERT(processed);
}

TEST(shared_futex_condvar_broadcast) {
    pman::SharedFutexMutex mutex;
    pman::SharedFutexCondVar cv;
    std::atomic<int> waiting{0};
    std::atomic<int> woken{0};
    bool go = false;

    auto worker = [&]() {
        mutex.lock();
        ++waiting;
        while (!go) {
            cv.wait(mutex);
        }
        ++woken;
        mutex.unlock();
    };

    std::thread t1(worker), t2(worker), t3(worker);

    // Wait for all threads to be waiting
    while (waiting < 3) {
        std::this_thread::sleep_for(10ms);
    }

    mutex.lock();
    go = true;
    cv.notify_all();
    mutex.unlock();

    t1.join();
    t2.join();
    t3.join();

    ASSERT_EQ(woken.load(), 3);
}

TEST(shared_futex_semaphore) {
    pman::SharedFutexSemaphore sem(2);

    ASSERT(sem.try_acquire());
    ASSERT(sem.try_acquire());
    ASSERT(!sem.try_acquire());  // Should fail, count is 0

    sem.release();
    ASSERT(sem.try_acquire());
}

// =============================================================================
// MessageQueue Tests
// =============================================================================

TEST(mq_send_receive) {
    const char* name = "/pman_test_mq";

    try { pman::PosixMessageQueue::unlink(name); } catch (...) {}

    pman::PosixMessageQueue::Options opts;
    opts.maxMessages = 10;
    opts.maxMessageSize = 256;

    pman::PosixMessageQueue mq(name, pman::SharedMemoryMode::Create, opts);

    const char* message = "Hello, message queue!";
    auto msgSpan = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(message),
        std::strlen(message) + 1
    );

    mq.send(msgSpan);

    std::array<std::byte, 256> buffer;
    std::size_t received = mq.receive(buffer);

    ASSERT(received > 0);
    ASSERT(std::strcmp(reinterpret_cast<const char*>(buffer.data()), message) == 0);

    pman::PosixMessageQueue::unlink(name);
}

TEST(mq_try_send_receive) {
    const char* name = "/pman_test_mq_try";

    try { pman::PosixMessageQueue::unlink(name); } catch (...) {}

    pman::PosixMessageQueue::Options opts;
    opts.maxMessages = 2;
    opts.maxMessageSize = 64;

    pman::PosixMessageQueue mq(name, pman::SharedMemoryMode::Create, opts);

    std::byte data[8] = {};

    ASSERT(mq.trySend(std::span<const std::byte>(data, 8)));
    ASSERT(mq.trySend(std::span<const std::byte>(data, 8)));
    // Queue is full
    ASSERT(!mq.trySend(std::span<const std::byte>(data, 8)));

    std::array<std::byte, 64> buffer;
    auto result = mq.tryReceive(buffer);
    ASSERT(result.has_value());

    result = mq.tryReceive(buffer);
    ASSERT(result.has_value());

    // Queue is empty
    result = mq.tryReceive(buffer);
    ASSERT(!result.has_value());

    pman::PosixMessageQueue::unlink(name);
}

TEST(mq_priority) {
    const char* name = "/pman_test_mq_prio";

    try { pman::PosixMessageQueue::unlink(name); } catch (...) {}

    pman::PosixMessageQueue::Options opts;
    opts.maxMessages = 10;
    opts.maxMessageSize = 64;

    pman::PosixMessageQueue mq(name, pman::SharedMemoryMode::Create, opts);

    // Send messages with different priorities
    const char* low = "low";
    const char* high = "high";

    mq.send(std::span<const std::byte>(reinterpret_cast<const std::byte*>(low), 4), 1);
    mq.send(std::span<const std::byte>(reinterpret_cast<const std::byte*>(high), 5), 10);

    std::array<std::byte, 64> buffer;
    unsigned int priority = 0;

    // High priority should come first
    mq.receive(buffer, &priority);
    ASSERT_EQ(priority, 10u);
    ASSERT(std::strcmp(reinterpret_cast<const char*>(buffer.data()), "high") == 0);

    mq.receive(buffer, &priority);
    ASSERT_EQ(priority, 1u);

    pman::PosixMessageQueue::unlink(name);
}

// =============================================================================
// UnixSocket Tests
// =============================================================================

TEST(unix_socket_stream) {
    const char* path = "/tmp/pman_test_unix_stream.sock";
    std::remove(path);

    pman::UnixSocket server(pman::UnixSocketType::Stream);
    server.bind(path);
    server.listen(5);

    std::thread clientThread([path]() {
        std::this_thread::sleep_for(50ms);
        pman::UnixSocket client(pman::UnixSocketType::Stream);
        client.connect(path);

        const char* msg = "Hello from client";
        client.send(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(msg),
            std::strlen(msg) + 1
        ));

        std::array<std::byte, 64> buffer;
        auto received = client.recv(buffer);
        ASSERT(received > 0);
    });

    auto clientConn = server.accept();
    ASSERT(clientConn.fd() >= 0);

    std::array<std::byte, 64> buffer;
    auto received = clientConn.recv(buffer);
    ASSERT(received > 0);
    ASSERT(std::strcmp(reinterpret_cast<const char*>(buffer.data()), "Hello from client") == 0);

    const char* reply = "Hello from server";
    clientConn.send(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(reply),
        std::strlen(reply) + 1
    ));

    clientThread.join();
    std::remove(path);
}

TEST(unix_socket_datagram) {
    const char* serverPath = "/tmp/pman_test_unix_dgram_server.sock";
    const char* clientPath = "/tmp/pman_test_unix_dgram_client.sock";
    std::remove(serverPath);
    std::remove(clientPath);

    pman::UnixSocket server(pman::UnixSocketType::Datagram);
    server.bind(serverPath);

    std::thread clientThread([serverPath, clientPath]() {
        pman::UnixSocket client(pman::UnixSocketType::Datagram);
        client.bind(clientPath);

        const char* msg = "Datagram message";
        client.sendTo(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(msg),
                std::strlen(msg) + 1
            ),
            serverPath
        );
    });

    std::array<std::byte, 64> buffer;
    std::string senderPath;
    auto received = server.recvFrom(buffer, senderPath);

    ASSERT(received > 0);
    ASSERT(std::strcmp(reinterpret_cast<const char*>(buffer.data()), "Datagram message") == 0);

    clientThread.join();
    std::remove(serverPath);
    std::remove(clientPath);
}

TEST(unix_socket_nonblocking) {
    const char* path = "/tmp/pman_test_unix_nonblock.sock";
    std::remove(path);

    pman::UnixSocket server(pman::UnixSocketType::Stream);
    server.bind(path);
    server.listen(5);
    server.setNonBlocking(true);

    // Should not block, returns socket (may throw or have fd < 0)
    try {
        auto result = server.accept();
        ASSERT(result.fd() < 0);  // No client connected
    } catch (...) {
        // Exception is also valid for EAGAIN/EWOULDBLOCK
    }

    std::remove(path);
}

TEST(unix_socket_send_fd) {
    const char* path = "/tmp/pman_test_unix_fd.sock";
    std::remove(path);

    pman::UnixSocket server(pman::UnixSocketType::Stream);
    server.bind(path);
    server.listen(5);

    std::thread clientThread([path]() {
        std::this_thread::sleep_for(50ms);
        pman::UnixSocket client(pman::UnixSocketType::Stream);
        client.connect(path);

        // Send stdin fd (always available)
        client.sendFd(STDIN_FILENO);
    });

    auto conn = server.accept();
    int receivedFd = conn.recvFd();

    // Should receive a valid fd
    ASSERT(receivedFd >= 0);
    ::close(receivedFd);

    clientThread.join();
    std::remove(path);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::printf("=== IPC Tests ===\n\n");
    // Tests run automatically via static initialization
    std::printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
    return testsFailed > 0 ? 1 : 0;
}
