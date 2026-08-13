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
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char a, unsigned char b) {
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
    const auto found = std::find_if(
        facts.processes.begin(), facts.processes.end(),
        [&](const ProcessInfo& info) { return equals_ignore_case(info.executable, payload.executable); });

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
    const auto found = std::find_if(
        facts.services.begin(), facts.services.end(),
        [&](const ServiceInfo& info) { return equals_ignore_case(info.name, payload.service_name); });

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
            result.observed = "not supported on this platform";
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
                } else {
                    return evaluate_registry(*rule, payload, facts);
                }
            },
            rule->payload));
    }

    return report;
}

std::size_t count_by_status(const ComplianceReport& report, CheckStatus status) {
    return static_cast<std::size_t>(
        std::count_if(report.results.begin(), report.results.end(),
                      [&](const CheckResult& result) { return result.status == status; }));
}

bool is_compliant(const ComplianceReport& report) {
    return count_by_status(report, CheckStatus::Fail) == 0;
}

}  // namespace lm::core
