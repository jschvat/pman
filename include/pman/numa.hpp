#pragma once

#include <vector>
#include <new>

#include "pman/topology.hpp"

namespace pman {

enum class MemoryPolicy {
    Default,
    Bind,
    Preferred,
    Interleave,
};

class ScopedMemoryPolicy {
public:
    ScopedMemoryPolicy(MemoryPolicy policy, std::vector<int> nodes = {});
    ~ScopedMemoryPolicy();

    ScopedMemoryPolicy(const ScopedMemoryPolicy&) = delete;
    ScopedMemoryPolicy& operator=(const ScopedMemoryPolicy&) = delete;

    ScopedMemoryPolicy(ScopedMemoryPolicy&&) noexcept = delete;
    ScopedMemoryPolicy& operator=(ScopedMemoryPolicy&&) noexcept = delete;

private:
    bool active_{false};
};

template <typename T>
class NumaAllocator {
public:
    using value_type = T;
    template <typename>
    friend class NumaAllocator;

    NumaAllocator() = default;
    explicit NumaAllocator(int node, MemoryPolicy policy = MemoryPolicy::Bind)
        : node_(node), policy_(policy) {}

    template <typename U>
    NumaAllocator(const NumaAllocator<U>& other) noexcept
        : node_(other.node()), policy_(other.policy()) {}

    [[nodiscard]] int node() const noexcept { return node_; }
    [[nodiscard]] MemoryPolicy policy() const noexcept { return policy_; }

    T* allocate(std::size_t n) {
        if (n > std::size_t(-1) / sizeof(T)) {
            throw std::bad_alloc();
        }
        ScopedMemoryPolicy guard(policy_, node_ >= 0 ? std::vector<int>{node_} : std::vector<int>{});
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }

    void deallocate(T* ptr, std::size_t /*count*/) noexcept {
        ::operator delete(ptr);
    }

    template <typename U>
    bool operator==(const NumaAllocator<U>& other) const noexcept {
        return node_ == other.node() && policy_ == other.policy();
    }

    template <typename U>
    bool operator!=(const NumaAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    int node_{-1};
    MemoryPolicy policy_{MemoryPolicy::Default};
};

}  // namespace pman
