#include "pman/topology.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace pman {
namespace {

constexpr const char* kNodeBasePath = "/sys/devices/system/node";
constexpr const char* kCpuBasePath = "/sys/devices/system/cpu";

std::vector<int> parseCpuList(const std::string& text) {
    std::vector<int> cpus;
    std::string token;
    std::stringstream ss(text);
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        auto dash = token.find('-');
        if (dash == std::string::npos) {
            cpus.push_back(std::stoi(token));
            continue;
        }
        int start = std::stoi(token.substr(0, dash));
        int end = std::stoi(token.substr(dash + 1));
        if (end < start) {
            std::swap(start, end);
        }
        for (int cpu = start; cpu <= end; ++cpu) {
            cpus.push_back(cpu);
        }
    }
    return cpus;
}

int readIntFile(const fs::path& path, int fallback = -1) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return fallback;
    }
    int value = fallback;
    file >> value;
    return value;
}

}  // namespace

SystemTopology SystemTopology::load() {
    SystemTopology topo;

    if (fs::exists(kNodeBasePath)) {
        for (const auto& entry : fs::directory_iterator(kNodeBasePath)) {
            if (!entry.is_directory()) {
                continue;
            }

            const auto name = entry.path().filename().string();
            if (name.rfind("node", 0) != 0) {
                continue;
            }

            int nodeId = std::stoi(name.substr(4));
            std::ifstream cpulist(entry.path() / "cpulist");
            if (!cpulist.is_open()) {
                continue;
            }

            std::string contents;
            std::getline(cpulist, contents);
            auto cpus = parseCpuList(contents);
            if (cpus.empty()) {
                continue;
            }

            std::sort(cpus.begin(), cpus.end());
            cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());

            topo.nodes_.push_back(NumaNodeInfo{
                .nodeId = nodeId,
                .cpus = cpus,
            });

            for (int cpu : cpus) {
                topo.cpuToNode_[cpu] = nodeId;
            }
        }
    }

    if (topo.nodes_.empty()) {
        unsigned fallbackCount = std::thread::hardware_concurrency();
        if (fallbackCount == 0) {
            fallbackCount = 1;
        }
        std::vector<int> cpus;
        cpus.reserve(fallbackCount);
        for (unsigned i = 0; i < fallbackCount; ++i) {
            cpus.push_back(static_cast<int>(i));
        }
        topo.nodes_.push_back(NumaNodeInfo{.nodeId = 0, .cpus = cpus});
        for (int cpu : cpus) {
            topo.cpuToNode_[cpu] = 0;
        }
    } else {
        std::sort(topo.nodes_.begin(), topo.nodes_.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.nodeId < rhs.nodeId;
        });
    }

    if (fs::exists(kCpuBasePath)) {
        for (const auto& entry : fs::directory_iterator(kCpuBasePath)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto name = entry.path().filename().string();
            if (name.rfind("cpu", 0) != 0 || name.size() <= 3) {
                continue;
            }

            int cpuId = -1;
            try {
                cpuId = std::stoi(name.substr(3));
            } catch (...) {
                continue;
            }

            fs::path topoPath = entry.path() / "topology";
            int coreId = readIntFile(topoPath / "core_id");
            int packageId = readIntFile(topoPath / "physical_package_id");

            std::vector<int> siblings;
            std::ifstream siblingsFile(topoPath / "thread_siblings_list");
            std::string contents;
            if (siblingsFile.is_open() && std::getline(siblingsFile, contents)) {
                siblings = parseCpuList(contents);
            }
            if (siblings.empty()) {
                siblings.push_back(cpuId);
            }

            int nodeId = -1;
            auto nodeIt = topo.cpuToNode_.find(cpuId);
            if (nodeIt != topo.cpuToNode_.end()) {
                nodeId = nodeIt->second;
            }

            topo.cpuInfos_.push_back(CpuInfo{
                .cpuId = cpuId,
                .nodeId = nodeId,
                .coreId = coreId,
                .packageId = packageId,
                .threadSiblings = siblings,
            });

            if (coreId >= 0) {
                auto& list = topo.coreToCpu_[coreId];
                list.insert(list.end(), siblings.begin(), siblings.end());
            }
        }
    }

    if (topo.cpuInfos_.empty()) {
        for (const auto& node : topo.nodes_) {
            for (int cpu : node.cpus) {
                topo.cpuInfos_.push_back(CpuInfo{
                    .cpuId = cpu,
                    .nodeId = node.nodeId,
                    .coreId = cpu,
                    .packageId = 0,
                    .threadSiblings = {cpu},
                });
                topo.coreToCpu_[cpu].push_back(cpu);
            }
        }
    }

    for (auto& [coreId, list] : topo.coreToCpu_) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    return topo;
}

const SystemTopology& SystemTopology::instance() {
    static const SystemTopology kInstance = SystemTopology::load();
    return kInstance;
}

const NumaNodeInfo* SystemTopology::node(int nodeId) const {
    auto it = std::find_if(nodes_.begin(), nodes_.end(), [nodeId](const NumaNodeInfo& info) {
        return info.nodeId == nodeId;
    });
    if (it == nodes_.end()) {
        return nullptr;
    }
    return &(*it);
}

CpuSet SystemTopology::cpuSetForNode(int nodeId) const {
    const auto* info = node(nodeId);
    if (!info) {
        throw std::out_of_range("unknown NUMA node");
    }
    CpuSet set;
    for (int cpu : info->cpus) {
        set.add(cpu);
    }
    return set;
}

std::vector<int> SystemTopology::cpuIds() const {
    std::vector<int> ids;
    for (const auto& node : nodes_) {
        ids.insert(ids.end(), node.cpus.begin(), node.cpus.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

const std::vector<int>& SystemTopology::siblingsForCore(int coreId) const {
    auto it = coreToCpu_.find(coreId);
    if (it == coreToCpu_.end()) {
        throw std::out_of_range("unknown core id");
    }
    return it->second;
}

const CpuInfo* SystemTopology::cpuInfo(int cpuId) const {
    auto it = std::find_if(cpuInfos_.begin(), cpuInfos_.end(), [cpuId](const CpuInfo& info) {
        return info.cpuId == cpuId;
    });
    if (it == cpuInfos_.end()) {
        return nullptr;
    }
    return &(*it);
}

}  // namespace pman
