#pragma once

#include <utility>

namespace pman {

class Pipe {
public:
    static Pipe create(bool cloexec = true);

    Pipe() = default;
    Pipe(int readFd, int writeFd);
    Pipe(Pipe&& other) noexcept;
    Pipe& operator=(Pipe&& other) noexcept;
    ~Pipe();

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    int readEnd() const noexcept { return readFd_; }
    int writeEnd() const noexcept { return writeFd_; }

    int releaseRead();
    int releaseWrite();

    void closeRead();
    void closeWrite();
    void close();

private:
    int readFd_{-1};
    int writeFd_{-1};
};

}  // namespace pman
