#include "lm/core/types.hpp"

namespace lm::core {

std::string to_string(Capability capability) {
    switch (capability) {
        case Capability::Resources: return "Resources";
        case Capability::Processes: return "Processes";
        case Capability::Services:  return "Services";
        case Capability::Registry:  return "Registry";
        case Capability::Network:   return "Network";
    }
    return "Unknown";
}

std::string to_string(AdapterType type) {
    switch (type) {
        case AdapterType::Unknown:  return "Unknown";
        case AdapterType::Ethernet: return "Ethernet";
        case AdapterType::WiFi:     return "Wi-Fi";
        case AdapterType::Loopback: return "Loopback";
        case AdapterType::Ppp:      return "PPP";
        case AdapterType::Tunnel:   return "Tunnel";
        case AdapterType::Modem:    return "Modem";
        case AdapterType::Other:    return "Other";
    }
    return "Unknown";
}

Capabilities platform_capabilities() {
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Processes).add(Capability::Services);
#ifdef _WIN32
    caps.add(Capability::Registry).add(Capability::Network);
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
