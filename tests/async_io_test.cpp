/// @file async_io_test.cpp
/// @brief Test async I/O operations: File and Socket

#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>

#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/file.hpp"
#include "pman/async/socket.hpp"
#include "pman/async/sleep.hpp"

using namespace pman::async;

namespace {

int tests_run = 0;
int tests_passed = 0;

void test(const char* name, bool result) {
    tests_run++;
    if (result) {
        tests_passed++;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        std::cerr << "  [FAIL] " << name << "\n";
    }
}

} // namespace

void test_file_write_read() {
    std::cout << "\n--- File Write/Read Test ---\n";

    EventLoop loop;
    bool write_ok = false;
    bool read_ok = false;
    std::string read_content;

    auto task = [&]() -> Task<void> {
        const std::string content = "Hello, async file I/O!";

        // Write file
        auto write_result = co_await writeFile("/tmp/pman_test.txt", content);
        write_ok = write_result.ok();

        // Read file back
        auto read_result = co_await readFile("/tmp/pman_test.txt");
        read_ok = read_result.ok();
        read_content = read_result.value;

        loop.stop();
    };

    task().start();
    loop.run();

    test("File write succeeded", write_ok);
    test("File read succeeded", read_ok);
    test("Read content matches written", read_content == "Hello, async file I/O!");

    // Cleanup
    unlink("/tmp/pman_test.txt");
}

void test_file_operations() {
    std::cout << "\n--- File Operations Test ---\n";

    EventLoop loop;
    bool created = false;
    bool written = false;
    bool seeked = false;
    bool read_correct = false;

    auto task = [&]() -> Task<void> {
        try {
            auto file = AsyncFile::open("/tmp/pman_ops_test.txt",
                O_RDWR | O_CREAT | O_TRUNC, 0644);
            created = true;

            // Write some data
            std::string data = "0123456789";
            auto write_res = co_await file.write(data);
            written = write_res.ok() && write_res.value == 10;

            // Seek to beginning
            auto seek_res = co_await file.seek(0);
            seeked = seek_res.ok() && seek_res.value == 0;

            // Read back
            auto read_res = co_await file.read(10);
            if (read_res.ok()) {
                std::string content(read_res.value.begin(), read_res.value.end());
                read_correct = (content == "0123456789");
            }

            co_await file.close();

        } catch (...) {
            // Handle errors
        }

        loop.stop();
    };

    task().start();
    loop.run();

    test("File created", created);
    test("Data written", written);
    test("Seek succeeded", seeked);
    test("Data read correctly", read_correct);

    // Cleanup
    unlink("/tmp/pman_ops_test.txt");
}

void test_socket_server_client() {
    std::cout << "\n--- Socket Server/Client Test ---\n";

    EventLoop loop;
    bool server_started = false;
    bool client_connected = false;
    bool data_sent = false;
    bool data_received = false;
    std::string received_data;

    // Server task
    auto server = [&]() -> Task<void> {
        try {
            auto listener = AsyncSocket::bind("127.0.0.1", 9999);
            auto listen_res = co_await listener.listen(1);
            server_started = listen_res.ok();

            auto maybe_client = co_await listener.accept(&loop);
            if (maybe_client) {
                auto client = std::move(*maybe_client);

                // Receive data
                auto recv_res = co_await client.recv(1024, &loop);
                if (recv_res.ok()) {
                    data_received = true;
                    received_data = std::string(recv_res.value.begin(), recv_res.value.end());
                }

                // Echo back
                co_await client.send(received_data, &loop);
            }

        } catch (...) {
            // Handle errors
        }
    };

    // Client task
    auto client = [&]() -> Task<void> {
        // Give server time to start
        co_await sleep(Duration::fromMillis(50), &loop);

        try {
            auto maybe_sock = co_await AsyncSocket::connect("127.0.0.1", 9999, &loop);
            if (maybe_sock) {
                client_connected = true;
                auto sock = std::move(*maybe_sock);

                // Send data
                const std::string message = "Hello Server!";
                auto send_res = co_await sock.send(message, &loop);
                data_sent = send_res.ok();

                // Receive echo
                co_await sock.recv(1024, &loop);
            }

        } catch (...) {
            // Handle errors
        }

        loop.stop();
    };

    server().start();
    client().start();

    loop.addTimer(std::chrono::seconds(5), [&]() {
        loop.stop();  // Timeout
    });

    loop.run();

    test("Server started", server_started);
    test("Client connected", client_connected);
    test("Data sent", data_sent);
    test("Data received", data_received);
    test("Received correct data", received_data == "Hello Server!");
}

void test_file_large_data() {
    std::cout << "\n--- File Large Data Test ---\n";

    EventLoop loop;
    bool write_ok = false;
    bool read_ok = false;
    bool size_correct = false;

    auto task = [&]() -> Task<void> {
        // Create 1MB of data
        std::string large_data(1024 * 1024, 'X');

        auto write_res = co_await writeFile("/tmp/pman_large_test.txt", large_data);
        write_ok = write_res.ok();

        auto read_res = co_await readFile("/tmp/pman_large_test.txt");
        read_ok = read_res.ok();
        size_correct = read_res.value.size() == large_data.size();

        loop.stop();
    };

    task().start();
    loop.run();

    test("Large file written", write_ok);
    test("Large file read", read_ok);
    test("Size matches", size_correct);

    // Cleanup
    unlink("/tmp/pman_large_test.txt");
}

void test_socket_multiple_messages() {
    std::cout << "\n--- Socket Multiple Messages Test ---\n";

    EventLoop loop;
    std::atomic<int> messages_received{0};
    bool all_sent = false;

    auto server = [&]() -> Task<void> {
        try {
            auto listener = AsyncSocket::bind("127.0.0.1", 10000);
            co_await listener.listen(1);

            auto maybe_client = co_await listener.accept(&loop);
            if (maybe_client) {
                auto client = std::move(*maybe_client);

                // Receive 3 messages
                for (int i = 0; i < 3; i++) {
                    auto recv_res = co_await client.recv(1024, &loop);
                    if (recv_res.ok() && !recv_res.value.empty()) {
                        messages_received++;
                    }
                }
            }

        } catch (...) {}
    };

    auto client = [&]() -> Task<void> {
        co_await sleep(Duration::fromMillis(50), &loop);

        try {
            auto maybe_sock = co_await AsyncSocket::connect("127.0.0.1", 10000, &loop);
            if (maybe_sock) {
                auto sock = std::move(*maybe_sock);

                // Send 3 messages
                for (int i = 0; i < 3; i++) {
                    std::string msg = "Message " + std::to_string(i);
                    co_await sock.send(msg, &loop);
                    co_await sleep(Duration::fromMillis(10), &loop);
                }
                all_sent = true;
            }

        } catch (...) {}

        // Give server time to receive all
        co_await sleep(Duration::fromMillis(100), &loop);
        loop.stop();
    };

    server().start();
    client().start();

    loop.addTimer(std::chrono::seconds(5), [&]() {
        loop.stop();
    });

    loop.run();

    test("All messages sent", all_sent);
    test("All messages received", messages_received == 3);
}

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              Async I/O Test Suite                            ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    try {
        test_file_write_read();
        test_file_operations();
        test_file_large_data();
        test_socket_server_client();
        test_socket_multiple_messages();
    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] Exception: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n========================================\n";
    std::cout << "RESULTS\n";
    std::cout << "========================================\n";
    std::cout << "Passed: " << tests_passed << "/" << tests_run << "\n";

    return tests_passed == tests_run ? 0 : 1;
}
