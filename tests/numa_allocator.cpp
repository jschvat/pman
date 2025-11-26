#include <vector>

#include "pman/numa.hpp"
#include "pman/topology.hpp"

int main() {
    const auto& topo = pman::SystemTopology::instance();
    int nodeId = topo.nodes().empty() ? -1 : topo.nodes().front().nodeId;

    pman::NumaAllocator<int> alloc(nodeId, pman::MemoryPolicy::Bind);
    std::vector<int, pman::NumaAllocator<int>> data(alloc);
    data.resize(16, 42);
    int sum = 0;
    for (int v : data) {
        sum += v;
    }
    return sum == 16 * 42 ? 0 : 1;
}
