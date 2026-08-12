#include <unistd.h>

#include <string>

#include "lm/platform/probes.hpp"

namespace lm::platform {

std::string local_host_name() {
    char buffer[256] = {};
    if (gethostname(buffer, sizeof(buffer) - 1) != 0) {
        return "unknown-host";
    }
    return std::string(buffer);
}

ProbeSet make_platform_probes() {
    return ProbeSet{};  // Task 8 fills this in.
}

}  // namespace lm::platform
