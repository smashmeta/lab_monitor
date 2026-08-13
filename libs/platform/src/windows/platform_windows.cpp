#include <windows.h>

#include <string>

#include "lm/platform/probes.hpp"

namespace lm::platform {

std::string local_host_name() {
    char buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(buffer);
    if (GetComputerNameA(buffer, &size) == 0) {
        return "unknown-host";
    }
    return std::string(buffer, size);
}

ProbeSet make_platform_probes() {
    ProbeSet probes;
    probes.resources = make_resource_probe();
    probes.processes = make_process_probe();
    probes.registry = make_registry_probe();
    // Services are still stubbed. HostProbes intersects capabilities with the
    // probes actually supplied, so leaving this null makes the client honestly
    // advertise that it cannot answer service rules.
    return probes;
}

}  // namespace lm::platform
