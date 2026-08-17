#include "lm/platform/probes.hpp"

#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace lm::platform {
namespace {

core::Capabilities intersect(core::Capabilities declared, const ProbeSet& probes) {
    core::Capabilities result;
    if (probes.resources && declared.has(core::Capability::Resources)) {
        result.add(core::Capability::Resources);
    }
    if (probes.processes && declared.has(core::Capability::Processes)) {
        result.add(core::Capability::Processes);
    }
    if (probes.services && declared.has(core::Capability::Services)) {
        result.add(core::Capability::Services);
    }
    if (probes.registry && declared.has(core::Capability::Registry)) {
        result.add(core::Capability::Registry);
    }
    if (probes.network && declared.has(core::Capability::Network)) {
        result.add(core::Capability::Network);
    }
    return result;
}

}  // namespace

HostProbes::HostProbes(core::HostId host_id, ProbeSet probes, core::Capabilities caps)
    : host_id_(std::move(host_id)), probes_(std::move(probes)), caps_(intersect(caps, probes_)) {}

core::ResourceSample HostProbes::sample_resources() {
    core::ResourceSample sample =
        probes_.resources ? probes_.resources->sample() : core::ResourceSample{};
    // Adapters ride the resource sample rather than the compliance report:
    // link state is a live reading, and this is the only message that already
    // travels on every tick. No rule references them, so unlike processes and
    // registry there is nothing to probe lazily against.
    if (caps_.has(core::Capability::Network)) {
        sample.adapters = probes_.network->enumerate();
    }
    return sample;
}

core::HostFacts HostProbes::collect(const core::TemplateBundle& bundle) {
    core::HostFacts facts;
    facts.host_id = host_id_;
    facts.resources = sample_resources();

    const std::vector<const core::Rule*> rules = core::rules_for(bundle, host_id_);

    bool needs_processes = false;
    bool needs_services = false;
    std::set<std::string> registry_keys;
    std::vector<const core::RegistryRule*> registry_rules;

    for (const core::Rule* rule : rules) {
        switch (core::kind_of(*rule)) {
            case core::RuleKind::Process:
                needs_processes = true;
                break;
            case core::RuleKind::Service:
                needs_services = true;
                break;
            case core::RuleKind::Registry: {
                const auto& payload = std::get<core::RegistryRule>(rule->payload);
                if (registry_keys.insert(core::registry_key(payload)).second) {
                    registry_rules.push_back(&payload);
                }
                break;
            }
        }
    }

    if (needs_processes && caps_.has(core::Capability::Processes)) {
        facts.processes = probes_.processes->enumerate();
    }
    if (needs_services && caps_.has(core::Capability::Services)) {
        facts.services = probes_.services->enumerate();
    }
    if (caps_.has(core::Capability::Registry)) {
        for (const core::RegistryRule* rule : registry_rules) {
            facts.registry.emplace(core::registry_key(*rule), probes_.registry->read(*rule));
        }
    }

    return facts;
}

}  // namespace lm::platform
