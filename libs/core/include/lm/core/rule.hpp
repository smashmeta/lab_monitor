#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "lm/core/types.hpp"
#include "lm/core/version.hpp"

namespace lm::core {

enum class RegistryHive { LocalMachine, CurrentUser, ClassesRoot, Users };
enum class RegistryMatch { Exists, Equals, Contains };

struct ProcessRule {
    std::string executable;
    friend bool operator==(const ProcessRule&, const ProcessRule&) = default;
};

struct ServiceRule {
    std::string service_name;
    std::optional<ServiceState> expected_state;
    friend bool operator==(const ServiceRule&, const ServiceRule&) = default;
};

struct RegistryRule {
    RegistryHive hive = RegistryHive::LocalMachine;
    std::string key_path;
    std::string value_name;
    RegistryMatch match = RegistryMatch::Exists;
    std::string expected_value;
    friend bool operator==(const RegistryRule&, const RegistryRule&) = default;
};

/// How many adapters must be up. Two payloads rather than one with a "which
/// form is this" flag, for the same reason `Rule` has no `kind` field: a flag
/// and the fields it selects are two sources of truth that can disagree.
struct AdapterCountRule {
    Comparison comparison = Comparison::AtLeast;
    /// Compared against the number of adapters whose link is Connected. Only
    /// Connected counts, via core::is_up() — the same definition the fleet
    /// list's "1 / 4" column uses.
    int count = 1;
    friend bool operator==(const AdapterCountRule&, const AdapterCountRule&) = default;
};

/// One named adapter must be in a given link state.
struct AdapterStateRule {
    /// Matched against NetworkAdapter::name — what Network Connections calls
    /// it, "smash-wifi" — case-insensitively, as process and service names are.
    /// Not the GUID: rules are written by people.
    std::string adapter_name;
    LinkState expected = LinkState::Connected;
    friend bool operator==(const AdapterStateRule&, const AdapterStateRule&) = default;
};

using RulePayload =
    std::variant<ProcessRule, ServiceRule, RegistryRule, AdapterCountRule, AdapterStateRule>;

struct Rule {
    RuleId id;
    std::string description;
    Presence expectation = Presence::MustBePresent;
    RulePayload payload;
    /// Process rules only. Ignored for service and registry rules.
    std::optional<VersionConstraint> version;
    friend bool operator==(const Rule&, const Rule&) = default;
};

/// Derives the rule kind from the payload variant. The kind is never stored,
/// so it cannot disagree with the payload.
[[nodiscard]] RuleKind kind_of(const Rule& rule);

[[nodiscard]] std::string to_string(RegistryHive hive);
[[nodiscard]] std::optional<RegistryHive> parse_registry_hive(std::string_view text);

/// Canonical lookup key for HostFacts::registry, of the form
/// "HKLM\<key_path>\\<value_name>".
[[nodiscard]] std::string registry_key(const RegistryRule& rule);

}  // namespace lm::core
