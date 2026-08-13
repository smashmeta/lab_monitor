#include "lm/core/types.hpp"

namespace lm::core {

std::string to_string(Capability capability) {
    switch (capability) {
        case Capability::Resources: return "Resources";
        case Capability::Processes: return "Processes";
        case Capability::Services:  return "Services";
        case Capability::Registry:  return "Registry";
    }
    return "Unknown";
}

Capabilities platform_capabilities() {
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Processes).add(Capability::Services);
#ifdef _WIN32
    caps.add(Capability::Registry);
#endif
    return caps;
}

Capability required_capability(RuleKind kind) {
    switch (kind) {
        case RuleKind::Process:  return Capability::Processes;
        case RuleKind::Service:  return Capability::Services;
        case RuleKind::Registry: return Capability::Registry;
    }
    return Capability::Resources;
}

}  // namespace lm::core
