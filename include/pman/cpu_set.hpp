#pragma once

#include <sched.h>

#include <initializer_list>
#include <stdexcept>
#include <vector>

namespace pman {

/**
 * Thin RAII helper around cpu_set_t so callers do not have to deal with the C macros.
 */
class CpuSet {
public:
    CpuSet() { CPU_ZERO(&mask_); }
    explicit CpuSet(std::initializer_list<int> cpus) : CpuSet() {
        for (int cpu : cpus) {
            add(cpu);
        }
    }

    static CpuSet range(int firstCpuInclusive, int lastCpuExclusive) {
        if (lastCpuExclusive < firstCpuInclusive) {
            throw std::invalid_argument("invalid CPU range");
        }
        CpuSet set;
        for (int cpu = firstCpuInclusive; cpu < lastCpuExclusive; ++cpu) {
            set.add(cpu);
        }
        return set;
    }

    void add(int cpu) {
        if (cpu < 0 || cpu >= static_cast<int>(CPU_SETSIZE)) {
            throw std::out_of_range("cpu index out of range");
        }
        CPU_SET(cpu, &mask_);
    }

    void remove(int cpu) {
        if (cpu < 0 || cpu >= static_cast<int>(CPU_SETSIZE)) {
            throw std::out_of_range("cpu index out of range");
        }
        CPU_CLR(cpu, &mask_);
    }

    void clear() { CPU_ZERO(&mask_); }

    [[nodiscard]] bool contains(int cpu) const {
        if (cpu < 0 || cpu >= static_cast<int>(CPU_SETSIZE)) {
            return false;
        }
        return CPU_ISSET(cpu, &mask_);
    }

    [[nodiscard]] bool empty() const {
        for (int cpu = 0; cpu < static_cast<int>(CPU_SETSIZE); ++cpu) {
            if (CPU_ISSET(cpu, &mask_)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::vector<int> toVector() const {
        std::vector<int> cpus;
        for (int cpu = 0; cpu < static_cast<int>(CPU_SETSIZE); ++cpu) {
            if (CPU_ISSET(cpu, &mask_)) {
                cpus.push_back(cpu);
            }
        }
        return cpus;
    }

    [[nodiscard]] const cpu_set_t* data() const { return &mask_; }
    [[nodiscard]] cpu_set_t* data() { return &mask_; }
    [[nodiscard]] std::size_t size() const { return CPU_SETSIZE; }

private:
    cpu_set_t mask_{};
};

}  // namespace pman
