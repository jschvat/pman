/// @file async_file_test.cpp
/// @brief Test async file operations

#include <iostream>
#include "pman/async/event_loop.hpp"
#include "pman/async/task.hpp"
#include "pman/async/file.hpp"

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

void test_file_large_data() {
    std::cout << "\n--- File Large Data Test ---\n";

    EventLoop loop;
    bool write_ok = false;
    bool read_ok = false;
    bool size_correct = false;

    auto task = [&]() -> Task<void> {
        // Create 100KB of data
        std::string large_data(100 * 1024, 'X');

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

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              Async File I/O Test Suite                       ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    try {
        test_file_write_read();
        test_file_operations();
        test_file_large_data();
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
