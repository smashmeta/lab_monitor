#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lm/core/types.hpp"
#include "lm/core/version.hpp"

namespace lm::core {

struct DiskUsage {
    std::string mount;
    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0;
    friend bool operator==(const DiskUsage&, const DiskUsage&) = default;

    [[nodiscard]] double used_percent() const {
        return total_bytes == 0
                   ? 0.0
                   : 100.0 * static_cast<double>(total_bytes - free_bytes) /
                         static_cast<double>(total_bytes);
    }
};

struct ResourceSample {
    double cpu_percent = 0.0;
    std::uint64_t mem_total_bytes = 0;
    std::uint64_t mem_used_bytes = 0;
    std::vector<DiskUsage> disks;
    friend bool operator==(const ResourceSample&, const ResourceSample&) = default;
};

struct ProcessInfo {
    std::string executable;
    std::optional<Version> version;
    friend bool operator==(const ProcessInfo&, const ProcessInfo&) = default;
};

struct ServiceInfo {
    std::string name;
    ServiceState state = ServiceState::Unknown;
    friend bool operator==(const ServiceInfo&, const ServiceInfo&) = default;
};

struct RegistryValue {
    bool exists = false;
    std::string data;
    /// Set when the read itself failed, as opposed to the value being absent.
    std::string error;
    friend bool operator==(const RegistryValue&, const RegistryValue&) = default;
};

struct HostFacts {
    HostId host_id;
    ResourceSample resources;
    std::vector<ProcessInfo> processes;
    std::vector<ServiceInfo> services;
    /// Keyed by registry_key(). Absent entries mean the rule was never probed.
    std::map<std::string, RegistryValue> registry;
    friend bool operator==(const HostFacts&, const HostFacts&) = default;
};

}  // namespace lm::core
