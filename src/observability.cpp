#include "pman/observability.hpp"

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace pman {
namespace {

long clockTicksPerSecond() {
    static const long ticks = ::sysconf(_SC_CLK_TCK);
    return ticks > 0 ? ticks : 100;
}

std::string readFile(const fs::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open " + path.string());
    }
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

ThreadStats parseThreadStatLine(const std::string& line) {
    ThreadStats stats;
    auto lparen = line.find('(');
    auto rparen = line.rfind(')');
    if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) {
        throw std::runtime_error("malformed stat line");
    }

    stats.tid = std::stoi(line.substr(0, lparen));
    stats.name = line.substr(lparen + 1, rparen - lparen - 1);

    std::istringstream iss(line.substr(rparen + 2));
    stats.state = '?';
    iss >> stats.state;

    // fields before utime (#14) and stime (#15)
    long long dummy;
    for (int i = 0; i < 11; ++i) {
        iss >> dummy;
    }
    iss >> stats.userTimeTicks;  // field 14
    iss >> stats.systemTimeTicks;  // field 15
    return stats;
}

void parseContextSwitches(const fs::path& statusPath, ThreadStats& stats) {
    std::ifstream file(statusPath);
    if (!file.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("voluntary_ctxt_switches", 0) == 0) {
            stats.voluntaryContextSwitches = std::stoll(line.substr(line.find(':') + 1));
        } else if (line.rfind("nonvoluntary_ctxt_switches", 0) == 0) {
            stats.involuntaryContextSwitches = std::stoll(line.substr(line.find(':') + 1));
        }
    }
}

ThreadStats readThreadStats(pid_t pid, pid_t tid) {
    fs::path base = "/proc";
    base /= std::to_string(pid);
    base /= "task";
    base /= std::to_string(tid);

    ThreadStats stats;
    try {
        stats = parseThreadStatLine(readFile(base / "stat"));
        parseContextSwitches(base / "status", stats);
    } catch (...) {
        stats.tid = -1;
    }
    return stats;
}

ProcessSnapshot parseProcessStat(const std::string& line) {
    ProcessSnapshot snapshot;
    auto lparen = line.find('(');
    auto rparen = line.rfind(')');
    if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen) {
        throw std::runtime_error("malformed process stat line");
    }

    snapshot.pid = std::stoi(line.substr(0, lparen));
    snapshot.name = line.substr(lparen + 1, rparen - lparen - 1);

    std::istringstream iss(line.substr(rparen + 2));
    char state;
    iss >> state;  // state

    long long dummy;
    for (int i = 0; i < 11; ++i) {
        iss >> dummy;
    }
    iss >> snapshot.userTimeTicks;
    iss >> snapshot.systemTimeTicks;

    for (int i = 0; i < 7; ++i) {
        iss >> dummy;
    }
    iss >> snapshot.startTimeTicks;  // field 22
    return snapshot;
}

std::vector<pid_t> enumerateThreads(pid_t pid) {
    std::vector<pid_t> tids;
    fs::path taskDir = fs::path("/proc") / std::to_string(pid) / "task";
    if (!fs::exists(taskDir)) {
        return tids;
    }
    for (const auto& entry : fs::directory_iterator(taskDir)) {
        if (!entry.is_directory()) {
            continue;
        }
        const auto name = entry.path().filename().string();
        try {
            tids.push_back(static_cast<pid_t>(std::stoi(name)));
        } catch (...) {
            continue;
        }
    }
    return tids;
}

pid_t effectivePid(pid_t pid) {
    if (pid <= 0) {
        return static_cast<pid_t>(::getpid());
    }
    return pid;
}

}  // namespace

ProcessSnapshot snapshotProcess(pid_t pid) {
    pid = effectivePid(pid);
    fs::path statPath = fs::path("/proc") / std::to_string(pid) / "stat";
    ProcessSnapshot snapshot = parseProcessStat(readFile(statPath));

    auto tids = enumerateThreads(pid);
    snapshot.threads.reserve(tids.size());
    for (pid_t tid : tids) {
        auto stats = readThreadStats(pid, tid);
        if (stats.tid >= 0) {
            snapshot.threads.push_back(std::move(stats));
        }
    }
    return snapshot;
}

ThreadStats snapshotThread(pid_t pid, pid_t tid) {
    pid = effectivePid(pid);
    if (tid <= 0) {
        tid = static_cast<pid_t>(::syscall(SYS_gettid));
    }
    auto stats = readThreadStats(pid, tid);
    if (stats.tid < 0) {
        throw std::runtime_error("failed to snapshot thread");
    }
    return stats;
}

}  // namespace pman
