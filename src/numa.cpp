#include "pman/numa.hpp"

#ifdef __linux__
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <system_error>
#include <vector>

namespace pman {
namespace {

#ifdef __linux__
int toMode(MemoryPolicy policy) {
    switch (policy) {
        case MemoryPolicy::Default:
            return MPOL_DEFAULT;
        case MemoryPolicy::Bind:
            return MPOL_BIND;
        case MemoryPolicy::Preferred:
            return MPOL_PREFERRED;
        case MemoryPolicy::Interleave:
            return MPOL_INTERLEAVE;
    }
    return MPOL_DEFAULT;
}

std::vector<unsigned long> buildNodeMask(const std::vector<int>& nodes) {
    if (nodes.empty()) {
        return {};
    }
    int maxNode = 0;
    for (int node : nodes) {
        if (node > maxNode) {
            maxNode = node;
        }
    }
    std::size_t wordCount = (maxNode / (sizeof(unsigned long) * 8)) + 1;
    std::vector<unsigned long> mask(wordCount, 0);
    for (int node : nodes) {
        if (node < 0) {
            continue;
        }
        std::size_t idx = static_cast<std::size_t>(node) / (sizeof(unsigned long) * 8);
        std::size_t bit = static_cast<std::size_t>(node) % (sizeof(unsigned long) * 8);
        mask[idx] |= (1UL << bit);
    }
    return mask;
}

void callSetMempolicy(int mode, unsigned long* mask, unsigned long maxnode) {
    long rc = syscall(SYS_set_mempolicy, mode, mask, maxnode);
    if (rc != 0) {
        throw std::system_error(errno, std::generic_category(), "set_mempolicy");
    }
}
#endif

}  // namespace

ScopedMemoryPolicy::ScopedMemoryPolicy(MemoryPolicy policy, std::vector<int> nodes) {
#ifdef __linux__
    if (policy == MemoryPolicy::Default) {
        return;
    }
    auto mask = buildNodeMask(nodes);
    unsigned long maxnode = mask.empty() ? 0 : static_cast<unsigned long>(mask.size() * sizeof(unsigned long) * 8);
    callSetMempolicy(toMode(policy), mask.empty() ? nullptr : mask.data(), maxnode);
    active_ = true;
#else
    (void)policy;
    (void)nodes;
#endif
}

ScopedMemoryPolicy::~ScopedMemoryPolicy() {
#ifdef __linux__
    if (active_) {
        callSetMempolicy(MPOL_DEFAULT, nullptr, 0);
    }
#endif
}

}  // namespace pman
