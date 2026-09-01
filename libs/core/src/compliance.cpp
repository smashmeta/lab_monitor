#include "lm/core/compliance.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "lm/core/json_path.hpp"

namespace lm::core {
namespace {

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
    // std::ranges::equal compares sizes itself for sized ranges, so the manual
    // length guard the two-iterator form needed is gone.
    return std::ranges::equal(lhs, rhs, [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
}

/// `expected` is carried only on failure: a passing check needs no explaining,
/// and repeating the expectation next to a tick is noise.
CheckResult resolve(const Rule& rule, bool present, std::string observed,
                    std::string expected = {}) {
    CheckResult result;
    result.rule_id = rule.id;
    result.observed = std::move(observed);
    const bool wanted = rule.expectation == Presence::MustBePresent;
    result.status = (present == wanted) ? CheckStatus::Pass : CheckStatus::Fail;
    if (result.status == CheckStatus::Fail) {
        result.message = std::move(expected);
    }
    return result;
}

/// Picks the phrasing that matches the rule's own Presence, so a MustBeAbsent
/// failure does not read backwards ("expected it to be running" on a rule that
/// wanted it gone).
std::string expected_text(const Rule& rule, std::string_view positive, std::string_view negative) {
    return "expected " +
           std::string(rule.expectation == Presence::MustBePresent ? positive : negative);
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

    const std::string wanted =
        expected_text(rule, payload.executable + " to be running",
                      payload.executable + " not to be running");

    if (found == facts.processes.end()) {
        return resolve(rule, false, "not running", wanted);
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
        if (!ok) {
            result.message = "expected version " + to_string(rule.version->op) + " " +
                             to_string(rule.version->value);
        }
        return result;
    }

    return resolve(rule, true, "running", wanted);
}

CheckResult evaluate_service(const Rule& rule, const ServiceRule& payload,
                             const HostFacts& facts) {
    const auto found = std::ranges::find_if(facts.services, [&](const ServiceInfo& info) {
        return equals_ignore_case(info.name, payload.service_name);
    });

    const std::string wanted =
        expected_text(rule, "service " + payload.service_name + " to be installed",
                      "service " + payload.service_name + " not to be installed");

    if (found == facts.services.end()) {
        return resolve(rule, false, "not installed", wanted);
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
        const bool ok = found->state == *payload.expected_state;
        result.status = ok ? CheckStatus::Pass : CheckStatus::Fail;
        if (!ok) {
            result.message = "expected state " + [&] {
                switch (*payload.expected_state) {
                    case ServiceState::Running: return std::string("Running");
                    case ServiceState::Stopped: return std::string("Stopped");
                    case ServiceState::Unknown: return std::string("Unknown");
                }
                return std::string("Unknown");
            }();
        }
        return result;
    }

    return resolve(rule, true, state, wanted);
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

    const std::string where = registry_key(payload);
    const RegistryValue& value = entry->second;
    if (!value.exists) {
        return resolve(rule, false, "value absent",
                       expected_text(rule, where + " to exist", where + " not to exist"));
    }

    switch (payload.match) {
        case RegistryMatch::Exists:
            return resolve(rule, true, value.data,
                           expected_text(rule, where + " to exist", where + " not to exist"));
        case RegistryMatch::Equals:
            return resolve(rule, value.data == payload.expected_value, value.data,
                           expected_text(rule, "the value to be \"" + payload.expected_value + "\"",
                                          "the value not to be \"" + payload.expected_value + "\""));
        case RegistryMatch::Contains:
            return resolve(
                rule, value.data.find(payload.expected_value) != std::string::npos, value.data,
                expected_text(rule, "the value to contain \"" + payload.expected_value + "\"",
                               "the value not to contain \"" + payload.expected_value + "\""));
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
    const bool ok = satisfies(connected, payload.comparison, payload.count);
    result.status = ok ? CheckStatus::Pass : CheckStatus::Fail;
    if (!ok) {
        // Without this the row reads "2 of 4 connected" and leaves the reader
        // to guess which rule that breaks -- the report this feature was fixed
        // for.
        result.message = "expected " + to_string(payload.comparison) + " " +
                         std::to_string(payload.count) + " connected";
    }
    return result;
}

CheckResult evaluate_adapter_state(const Rule& rule, const AdapterStateRule& payload,
                                    const HostFacts& facts) {
    const auto found = std::ranges::find_if(
        facts.resources.adapters, [&](const NetworkAdapter& adapter) {
            return equals_ignore_case(adapter.name, payload.adapter_name);
        });

    const std::string wanted = expected_text(
        rule, "\"" + payload.adapter_name + "\" to be " + to_string(payload.expected),
        "\"" + payload.adapter_name + "\" not to be " + to_string(payload.expected));

    if (found == facts.resources.adapters.end()) {
        // Fail rather than Error: the adapter genuinely is not in the state the
        // rule asked for, and "the NIC was removed" is exactly what a fleet
        // check should catch. Error is for "could not tell".
        return resolve(rule, false, "no adapter named \"" + payload.adapter_name + "\"", wanted);
    }

    const bool matches = found->link == payload.expected;
    // MustBeAbsent inverts it: "the guest adapter must NOT be connected" is as
    // reasonable a rule as its opposite, and resolve() already means exactly
    // that for every other kind.
    return resolve(rule, matches, to_string(found->link), wanted);
}

/// Shared by both DDS evaluators: finds what the probe reported for this
/// topic, or explains why nothing can be said about it.
///
/// Every branch here is an Error rather than a Fail. "The probe never ran",
/// "the type was never advertised" and "nobody has published yet" are all
/// statements about the *check*, not about the machine — the same distinction
/// that makes an unprobed registry value an Error.
const DdsTopicSample* find_topic(const HostFacts& facts, std::uint32_t domain_id,
                                 const std::string& topic_name) {
    const auto entry = facts.dds.find(dds_key(domain_id, topic_name));
    return entry == facts.dds.end() ? nullptr : &entry->second;
}

std::string on_domain(std::uint32_t domain_id, const std::string& topic_name) {
    return "\"" + topic_name + "\" on domain " + std::to_string(domain_id);
}

CheckResult evaluate_dds_topic(const Rule& rule, const DdsTopicRule& payload,
                               const HostFacts& facts) {
    const DdsTopicSample* sample = find_topic(facts, payload.domain_id, payload.topic_name);
    if (sample == nullptr) {
        return error(rule, "not probed", "the DDS bus was not read on this host");
    }
    if (!sample->error.empty()) {
        return error(rule, "look failed", sample->error);
    }

    // Presence of a topic needs no sample and no type: discovery alone answers
    // it, which is why this rule still works against a publisher that describes
    // nothing about its data.
    return resolve(rule, sample->topic_found,
                   sample->topic_found ? "present on the bus" : "not on the bus",
                   expected_text(rule, on_domain(payload.domain_id, payload.topic_name) + " to exist",
                                  on_domain(payload.domain_id, payload.topic_name) +
                                      " not to exist"));
}

/// Names what a JSON value actually is, for the message when a numeric
/// comparison meets something that is not a number.
std::string document_kind(const nlohmann::json& value) {
    if (value.is_string()) return "text";
    if (value.is_boolean()) return "a true/false value";
    if (value.is_array()) return "a sequence";
    if (value.is_object()) return "a structure";
    if (value.is_null()) return "nothing";
    return "a value of another kind";
}

/// Renders a JSON value the way a person would read it, so `observed` says
/// `Ready` and `2` rather than `"Ready"` and `2.0`.
std::string readable(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<std::int64_t>());
    }
    if (value.is_number_float()) {
        std::string text = std::to_string(value.get<double>());
        // Trim the trailing zeros std::to_string always produces: "12.000000"
        // is the same number as "12" and reads far better on a wall display.
        while (text.size() > 1 && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
        return text;
    }
    return value.dump();
}

/// What the report shows for the values a path addressed.
///
/// One value reads as itself, exactly as it always did. Several are listed, and
/// capped: a cart with forty lines would otherwise put forty skus on a fleet
/// row, and the count at the front is the part worth reading anyway.
std::string observed_text(const std::vector<nlohmann::json>& values) {
    if (values.size() == 1) {
        return readable(values.front());
    }
    constexpr std::size_t kMaxListed = 5;
    std::string text = std::to_string(values.size()) + " values: ";
    for (std::size_t i = 0; i < values.size() && i < kMaxListed; ++i) {
        if (i > 0) {
            text += ", ";
        }
        text += readable(values[i]);
    }
    if (values.size() > kMaxListed) {
        text += ", +" + std::to_string(values.size() - kMaxListed) + " more";
    }
    return text;
}

/// Whether one value satisfies the rule, or why it could not be compared.
///
/// Split out of evaluate_dds_value() when `[*]` arrived: the decision is now
/// made once per addressed value rather than once per rule, and "could not
/// compare this one" has to be answerable separately from "this one did not
/// match" so that a single odd member of a sequence does not turn a real answer
/// into an Error.
std::expected<bool, std::string> matches(const nlohmann::json& value, const DdsValueRule& payload) {
    const bool numeric_match = payload.match == DdsMatch::AtLeast || payload.match == DdsMatch::AtMost;
    if (numeric_match || (payload.match == DdsMatch::Equals && value.is_number())) {
        if (!value.is_number()) {
            return std::unexpected("expected a number at " + payload.path + " to compare against " +
                                   payload.expected_value + ", found " + document_kind(value));
        }
        double expected = 0.0;
        try {
            expected = std::stod(payload.expected_value);
        } catch (const std::exception&) {
            return std::unexpected("the rule's expected value \"" + payload.expected_value +
                                   "\" is not a number");
        }
        const double observed = value.get<double>();
        return payload.match == DdsMatch::AtLeast  ? observed >= expected
               : payload.match == DdsMatch::AtMost ? observed <= expected
                                                   : observed == expected;
    }

    // Textual from here: Equals against a non-number, and Contains.
    const std::string observed = readable(value);
    return payload.match == DdsMatch::Contains
               ? observed.find(payload.expected_value) != std::string::npos
               : observed == payload.expected_value;
}

CheckResult evaluate_dds_value(const Rule& rule, const DdsValueRule& payload,
                               const HostFacts& facts) {
    const DdsTopicSample* sample = find_topic(facts, payload.domain_id, payload.topic_name);
    if (sample == nullptr) {
        return error(rule, "not probed", "the DDS bus was not read on this host");
    }
    if (!sample->error.empty()) {
        return error(rule, "look failed", sample->error);
    }
    if (!sample->topic_found) {
        // A missing topic *is* a finding, the same way a renamed NIC is: the
        // rule asked about data that is meant to be on the bus and is not.
        CheckResult result;
        result.rule_id = rule.id;
        result.status = CheckStatus::Fail;
        result.observed = "topic not on the bus";
        result.message = "expected " + on_domain(payload.domain_id, payload.topic_name) +
                         " to be publishing";
        return result;
    }
    if (!sample->has_sample) {
        // The topic is there but nothing has been published since we started
        // listening. Nothing is wrong with the machine, and nothing can be
        // said about the value.
        return error(rule, "no sample yet",
                     "the topic is on the bus but nothing has been published on it");
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(sample->json);
    } catch (const std::exception& parse_error) {
        return error(rule, "unreadable sample",
                     std::string("the sample could not be read as JSON: ") + parse_error.what());
    }

    const auto found = resolve_all(document, payload.path);
    if (!found.has_value()) {
        // A malformed path is the author's mistake and a missing field is a
        // fact about the data, but neither one lets the rule be answered, so
        // both are Errors that quote the reason verbatim.
        return error(rule, payload.path.empty() ? "(whole sample)" : payload.path,
                     found.error().message);
    }

    const bool plural = payload.path.find("[*]") != std::string::npos;
    const std::string expectation = "expected " + std::string(plural ? "any " : "") + payload.path +
                                    " " + to_string(payload.match) + " " + payload.expected_value;

    CheckResult result;
    result.rule_id = rule.id;

    if (found->empty()) {
        // Only reachable through `[*]` over an empty sequence, and it is a
        // finding rather than a fault: nothing in the cart has that sku,
        // because there is nothing in the cart.
        result.status = CheckStatus::Fail;
        result.observed = "no elements";
        result.message = expectation;
        return result;
    }

    result.observed = observed_text(*found);

    // Any, not all. A singular path yields exactly one value, so this is the
    // same question it always asked; a `[*]` path asks it of every element and
    // passes on the first that answers yes.
    std::optional<std::string> first_error;
    std::size_t comparable = 0;
    for (const nlohmann::json& value : *found) {
        const std::expected<bool, std::string> matched = matches(value, payload);
        if (!matched.has_value()) {
            if (!first_error.has_value()) {
                first_error = matched.error();
            }
            continue;
        }
        ++comparable;
        if (*matched) {
            result.status = CheckStatus::Pass;
            return result;
        }
    }

    // Nothing could be compared at all -- a numeric rule against text, say.
    // Nothing is wrong with the machine and the rule cannot be answered, which
    // is an Error rather than a Fail. One comparable value is enough to make it
    // a real answer: a sequence with a stray non-numeric member still tells you
    // whether any member matched.
    if (comparable == 0 && first_error.has_value()) {
        return error(rule, result.observed, *first_error);
    }

    result.status = CheckStatus::Fail;
    result.message = expectation;
    return result;
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
                } else if constexpr (std::is_same_v<T, AdapterStateRule>) {
                    return evaluate_adapter_state(*rule, payload, facts);
                } else if constexpr (std::is_same_v<T, DdsTopicRule>) {
                    return evaluate_dds_topic(*rule, payload, facts);
                } else {
                    return evaluate_dds_value(*rule, payload, facts);
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

double ComplianceSummary::passed_ratio() const {
    if (checked() == 0) {
        return 1.0;
    }
    return static_cast<double>(passed) / static_cast<double>(checked());
}

ComplianceSummary summarise(const ComplianceReport& report) {
    ComplianceSummary summary;
    summary.passed = count_by_status(report, CheckStatus::Pass);
    summary.failing = count_by_status(report, CheckStatus::Fail);
    summary.errors = count_by_status(report, CheckStatus::Error);
    summary.not_applicable = count_by_status(report, CheckStatus::NotApplicable);
    return summary;
}

}  // namespace lm::core
