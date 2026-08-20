#include "lm/core/types.hpp"

namespace lm::core {

std::string to_string(Capability capability) {
    switch (capability) {
        case Capability::Resources: return "Resources";
        case Capability::Processes: return "Processes";
        case Capability::Services:  return "Services";
        case Capability::Registry:  return "Registry";
        case Capability::Network:   return "Network";
        case Capability::Dds:       return "DDS";
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

std::string to_string(LinkState state) {
    switch (state) {
        case LinkState::Unknown:      return "Unknown";
        case LinkState::Connected:    return "Up";
        case LinkState::NoMedia:      return "No link";
        case LinkState::Disconnected: return "Disconnected";
        case LinkState::Connecting:   return "Connecting";
        case LinkState::Disabled:     return "Disabled";
        case LinkState::Faulted:      return "Faulted";
    }
    return "Unknown";
}

std::string to_string(Comparison comparison) {
    switch (comparison) {
        case Comparison::AtLeast: return "at least";
        case Comparison::Exactly: return "exactly";
        case Comparison::AtMost:  return "at most";
    }
    return "at least";
}

std::string to_string(DdsMatch match) {
    switch (match) {
        case DdsMatch::Equals:   return "equal to";
        case DdsMatch::Contains: return "containing";
        case DdsMatch::AtLeast:  return "at least";
        case DdsMatch::AtMost:   return "at most";
    }
    return "equal to";
}

bool satisfies(int observed, Comparison comparison, int expected) {
    switch (comparison) {
        case Comparison::AtLeast: return observed >= expected;
        case Comparison::Exactly: return observed == expected;
        case Comparison::AtMost:  return observed <= expected;
    }
    return false;
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
        case RuleKind::Network:  return Capability::Network;
        case RuleKind::Dds:      return Capability::Dds;
    }
    return Capability::Resources;
}

}  // namespace lm::core
