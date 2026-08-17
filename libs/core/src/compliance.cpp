#include "lm/core/compliance.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace lm::core {
namespace {

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
    // std::ranges::equal compares sizes itself for sized ranges, so the manual
    // length guard the two-iterator form needed is gone.
    return std::ranges::equal(lhs, rhs, [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

CheckResult resolve(const Rule& rule, bool present, std::string observed) {
    CheckResult result;
    result.rule_id = rule.id;
    result.observed = std::move(observed);
    const bool wanted = rule.expectation == Presence::MustBePresent;
    result.status = (present == wanted) ? CheckStatus::Pass : CheckStatus::Fail;
    return result;
}

CheckResult error(const Rule& rule, std::string observed, std::string message) {
    CheckResult result;
    result.rule_id = rule.id;
    result.status = CheckStatus::Error;
    result.observed = std::move(observed);
    result.message = std::move(message);
    return result;
}

CheckResult evaluate_process(const Rule& rule, const ProcessRule& payload,
                             const HostFacts& facts) {
    const auto found = std::ranges::find_if(facts.processes, [&](const ProcessInfo& info) {
        return equals_ignore_case(info.executable, payload.executable);
    });

    if (found == facts.processes.end()) {
        return resolve(rule, false, "not running");
    }

    // A version constraint only qualifies presence. For MustBeAbsent the process
    // being there is already a failure, whatever its version.
    if (rule.version && rule.expectation == Presence::MustBePresent) {
        if (!found->version) {
            return error(rule, "running, version unreadable",
                         "process found but its version could not be determined");
        }
        const bool ok = satisfies(*found->version, *rule.version);
        CheckResult result;
        result.rule_id = rule.id;
        result.observed = "running, version " + to_string(*found->version);
        result.status = ok ? CheckStatus::Pass : CheckStatus::Fail;
        return result;
    }

    return resolve(rule, true, "running");
}

CheckResult evaluate_service(const Rule& rule, const ServiceRule& payload,
                             const HostFacts& facts) {
    const auto found = std::ranges::find_if(facts.services, [&](const ServiceInfo& info) {
        return equals_ignore_case(info.name, payload.service_name);
    });

    if (found == facts.services.end()) {
        return resolve(rule, false, "not installed");
    }

    const std::string state = [&] {
        switch (found->state) {
            case ServiceState::Running: return "Running";
            case ServiceState::Stopped: return "Stopped";
            case ServiceState::Unknown: return "Unknown";
        }
        return "Unknown";
    }();

    if (payload.expected_state && rule.expectation == Presence::MustBePresent) {
        CheckResult result;
        result.rule_id = rule.id;
        result.observed = state;
        result.status =
            (found->state == *payload.expected_state) ? CheckStatus::Pass : CheckStatus::Fail;
        return result;
    }

    return resolve(rule, true, state);
}

CheckResult evaluate_registry(const Rule& rule, const RegistryRule& payload,
                              const HostFacts& facts) {
    const auto entry = facts.registry.find(registry_key(payload));
    if (entry == facts.registry.end()) {
        return error(rule, "not probed", "registry value was not read on this host");
    }
    if (!entry->second.error.empty()) {
        return error(rule, "read failed", entry->second.error);
    }

    const RegistryValue& value = entry->second;
    if (!value.exists) {
        return resolve(rule, false, "value absent");
    }

    switch (payload.match) {
        case RegistryMatch::Exists:
            return resolve(rule, true, value.data);
        case RegistryMatch::Equals:
            return resolve(rule, value.data == payload.expected_value, value.data);
        case RegistryMatch::Contains:
            return resolve(rule, value.data.find(payload.expected_value) != std::string::npos,
                           value.data);
    }
    return error(rule, value.data, "unhandled registry match mode");
}

CheckResult evaluate_adapter_count(const Rule& rule, const AdapterCountRule& payload,
                                    const HostFacts& facts) {
    const auto connected = static_cast<int>(std::ranges::count_if(
        facts.resources.adapters,
        [](const NetworkAdapter& adapter) { return is_up(adapter.link); }));

    CheckResult result;
    result.rule_id = rule.id;
    result.observed = std::to_string(connected) + " of " +
                      std::to_string(facts.resources.adapters.size()) + " connected";
    // The comparison carries the direction on its own, so Presence would only
    // be a second, contradictable way of saying the same thing. Ignored here,
    // as the version constraint is for service and registry rules.
    result.status = satisfies(connected, payload.comparison, payload.count) ? CheckStatus::Pass
                                                                             : CheckStatus::Fail;
    return result;
}

CheckResult evaluate_adapter_state(const Rule& rule, const AdapterStateRule& payload,
                                    const HostFacts& facts) {
    const auto found = std::ranges::find_if(
        facts.resources.adapters, [&](const NetworkAdapter& adapter) {
            return equals_ignore_case(adapter.name, payload.adapter_name);
        });

    if (found == facts.resources.adapters.end()) {
        // Fail rather than Error: the adapter genuinely is not in the state the
        // rule asked for, and "the NIC was removed" is exactly what a fleet
        // check should catch. Error is for "could not tell".
        return resolve(rule, false, "no adapter named \"" + payload.adapter_name + "\"");
    }

    const bool matches = found->link == payload.expected;
    // MustBeAbsent inverts it: "the guest adapter must NOT be connected" is as
    // reasonable a rule as its opposite, and resolve() already means exactly
    // that for every other kind.
    return resolve(rule, matches, to_string(found->link));
}

}  // namespace

ComplianceReport evaluate(const TemplateBundle& bundle, const HostFacts& facts,
                          Capabilities caps) {
    ComplianceReport report;
    report.host_id = facts.host_id;
    report.applied_revision = bundle.revision;

    for (const Rule* rule : rules_for(bundle, facts.host_id)) {
        if (!caps.has(required_capability(kind_of(*rule)))) {
            CheckResult result;
            result.rule_id = rule->id;
            result.status = CheckStatus::NotApplicable;
            // Deliberately does not claim an OS limitation: a capability can be
            // absent either because the platform cannot serve it (registry on
            // Linux) or because its probe is not implemented yet. evaluate()
            // cannot tell those apart, so it reports what it actually knows.
            result.observed =
                "not checked: client does not report the " +
                to_string(required_capability(kind_of(*rule))) + " capability";
            report.results.push_back(std::move(result));
            continue;
        }

        report.results.push_back(std::visit(
            [&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, ProcessRule>) {
                    return evaluate_process(*rule, payload, facts);
                } else if constexpr (std::is_same_v<T, ServiceRule>) {
                    return evaluate_service(*rule, payload, facts);
                } else if constexpr (std::is_same_v<T, RegistryRule>) {
                    return evaluate_registry(*rule, payload, facts);
                } else if constexpr (std::is_same_v<T, AdapterCountRule>) {
                    return evaluate_adapter_count(*rule, payload, facts);
                } else {
                    return evaluate_adapter_state(*rule, payload, facts);
                }
            },
            rule->payload));
    }

    return report;
}

std::size_t count_by_status(const ComplianceReport& report, CheckStatus status) {
    return static_cast<std::size_t>(std::ranges::count(report.results, status, &CheckResult::status));
}

bool is_compliant(const ComplianceReport& report) {
    return count_by_status(report, CheckStatus::Fail) == 0;
}

}  // namespace lm::core
