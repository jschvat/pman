#pragma once

#include <cstddef>
#include <optional>

namespace pman {

struct MemoryLockOptions {
    bool lockCurrent{true};
    bool lockFuture{true};
};

void lockMemory(const MemoryLockOptions& options = {});
void unlockMemory();

void adviseWillNeed(void* addr, std::size_t length);
void enableHugePages(void* addr, std::size_t length);

void prefaultStack(std::size_t bytes);
void setGuardSize(std::size_t guardBytes);

}  // namespace pman
