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
    return ProbeSet{};  // Task 8 fills this in.
}

}  // namespace lm::platform
