#include <fstream>
#include <string>

#include "pman/process_builder.hpp"

int main() {
#ifdef __linux__
    const char* cgroup = "/sys/fs/cgroup";
    std::ifstream file(std::string(cgroup) + "/cgroup.controllers");
    if (!file.is_open()) {
        return 0;  // skip if cgroups not available
    }

    pman::ProcessBuilder builder("/bin/sleep");
    builder.arg("1");
    builder.cgroup(cgroup);
    auto handle = builder.spawn();
    handle.wait();
#endif
    return 0;
}
