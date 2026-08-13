#include "lm/core/types.hpp"

namespace lm::core {

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
