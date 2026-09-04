#include "lm/platform/probes.hpp"

#include <cstdint>
#include <map>
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
    if (probes.dds && declared.has(core::Capability::Dds)) {
        result.add(core::Capability::Dds);
    }
    // Scripts and Elevated pass straight through. This function exists so a
    // null probe can never advertise a capability it cannot serve, and neither
    // of these is served by a probe: the script runner is owned by the client's
    // worker, and elevation is a property of the process token. Dropping them
    // for want of a ProbeSet member would silently strip them from the
    // announce, which is the only carrier a client has for them.
    if (declared.has(core::Capability::Scripts)) {
        result.add(core::Capability::Scripts);
    }
    if (declared.has(core::Capability::Elevated)) {
        result.add(core::Capability::Elevated);
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
    // Deduplicated: several rules commonly read one topic -- "the basket
    // exists", "it holds 2 items", "its status is Ready" -- and each look
    // costs a DDS participant and a discovery wait, so they get one between
    // them. std::map rather than a vector for the same reason registry keys
    // use a set.
    std::map<std::string, std::pair<std::uint32_t, std::string>> dds_topics;

    const auto want_topic = [&](std::uint32_t domain_id, const std::string& topic_name) {
        dds_topics.try_emplace(core::dds_key(domain_id, topic_name), domain_id, topic_name);
    };

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
            case core::RuleKind::Dds:
                if (const auto* topic = std::get_if<core::DdsTopicRule>(&rule->payload)) {
                    want_topic(topic->domain_id, topic->topic_name);
                } else {
                    const auto& value = std::get<core::DdsValueRule>(rule->payload);
                    want_topic(value.domain_id, value.topic_name);
                }
                break;
            case core::RuleKind::Network:
                // Nothing to gather. Adapters ride the resource sample, which
                // has already been taken above, so unlike every other kind
                // there is nothing to probe lazily against. Listed rather than
                // left to a default so that adding a kind is a compile error
                // here -- which is exactly how this case came to be written.
                break;
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
    if (caps_.has(core::Capability::Dds)) {
        for (const auto& [key, target] : dds_topics) {
            const auto& [domain_id, topic_name] = target;
            facts.dds.emplace(key, probes_.dds->look(domain_id, topic_name));
        }
    }

    return facts;
}

}  // namespace lm::platform
