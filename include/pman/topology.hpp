#pragma once

#include <unordered_map>
#include <vector>

#include "pman/cpu_set.hpp"

namespace pman {

struct CpuInfo {
    int cpuId{-1};
    int nodeId{-1};
    int coreId{-1};
    int packageId{-1};
    std::vector<int> threadSiblings;
};

struct NumaNodeInfo {
    int nodeId{-1};
    std::vector<int> cpus;
};

class SystemTopology {
public:
    static SystemTopology load();
    static const SystemTopology& instance();

    [[nodiscard]] const std::vector<NumaNodeInfo>& nodes() const noexcept { return nodes_; }
    [[nodiscard]] const NumaNodeInfo* node(int nodeId) const;
    [[nodiscard]] CpuSet cpuSetForNode(int nodeId) const;
    [[nodiscard]] std::vector<int> cpuIds() const;
    [[nodiscard]] const std::vector<int>& siblingsForCore(int coreId) const;
    [[nodiscard]] const CpuInfo* cpuInfo(int cpuId) const;

private:
    std::vector<NumaNodeInfo> nodes_;
    std::vector<CpuInfo> cpuInfos_;
    std::unordered_map<int, std::vector<int>> coreToCpu_;
    std::unordered_map<int, int> cpuToNode_;
};

}  // namespace pman
