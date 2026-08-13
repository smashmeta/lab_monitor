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
    // processes, services and registry arrive in a later iteration; HostProbes
    // intersects capabilities with the probes actually supplied, so the client
    // honestly advertises resources only.
    return probes;
}

}  // namespace lm::platform
