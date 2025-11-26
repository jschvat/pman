#include <cassert>

#include "pman/thread_pool.hpp"
#include "pman/topology.hpp"

int main() {
    const auto& topo = pman::SystemTopology::instance();
    if (topo.nodes().empty()) {
        return 1;
    }

    const auto& node = topo.nodes().front();
    if (node.cpus.empty()) {
        return 2;
    }

    pman::ThreadAttributes attrs;
    attrs.numaNode = node.nodeId;
    pman::ManagedThread thread("topology-thread", [] {}, attrs);
    thread.join();

    pman::ThreadPool pool{2};
    pool.pinWorkersToNode(node.nodeId);
    pool.distributeByTopology(topo);

    return 0;
}
