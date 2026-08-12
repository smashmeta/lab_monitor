#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"
#include "lm/core/template_bundle.hpp"

namespace lm::platform {

class IResourceProbe {
public:
    virtual ~IResourceProbe() = default;
    virtual core::ResourceSample sample() = 0;
};

class IProcessProbe {
public:
    virtual ~IProcessProbe() = default;
    virtual std::vector<core::ProcessInfo> enumerate() = 0;
};

class IServiceProbe {
public:
    virtual ~IServiceProbe() = default;
    virtual std::vector<core::ServiceInfo> enumerate() = 0;
};

class IRegistryProbe {
public:
    virtual ~IRegistryProbe() = default;
    virtual core::RegistryValue read(const core::RegistryRule& rule) = 0;
};

/// Null members mean the capability is unavailable on this platform.
struct ProbeSet {
    std::unique_ptr<IResourceProbe> resources;
    std::unique_ptr<IProcessProbe> processes;
    std::unique_ptr<IServiceProbe> services;
    std::unique_ptr<IRegistryProbe> registry;
};

/// Assembles HostFacts snapshots. Probes lazily: only the categories the host's
/// own rules actually reference are queried.
class HostProbes {
public:
    /// Capabilities are intersected with the probes that were actually supplied,
    /// so a null probe can never advertise a capability it cannot serve.
    HostProbes(core::HostId host_id, ProbeSet probes, core::Capabilities caps);

    [[nodiscard]] core::HostFacts collect(const core::TemplateBundle& bundle);

    /// Resource-only sampling for the fast 2 s tick.
    [[nodiscard]] core::ResourceSample sample_resources();

    [[nodiscard]] core::Capabilities capabilities() const { return caps_; }
    [[nodiscard]] const core::HostId& host_id() const { return host_id_; }

private:
    core::HostId host_id_;
    ProbeSet probes_;
    core::Capabilities caps_;
};

/// The machine's hostname, used as the client identifier.
[[nodiscard]] std::string local_host_name();

/// Builds the probe set for the platform this binary was compiled for.
[[nodiscard]] ProbeSet make_platform_probes();

/// Builds the resource probe for this platform. Stateful: CPU load is computed
/// as a delta against the previous call, so the first sample reports 0 %.
[[nodiscard]] std::unique_ptr<IResourceProbe> make_resource_probe();

}  // namespace lm::platform
