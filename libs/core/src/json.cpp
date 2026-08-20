#include "lm/core/json.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace lm::core {
namespace {

std::string presence_to_string(Presence value) {
    return value == Presence::MustBePresent ? "MustBePresent" : "MustBeAbsent";
}

Presence presence_from_string(const std::string& text) {
    if (text == "MustBePresent") return Presence::MustBePresent;
    if (text == "MustBeAbsent") return Presence::MustBeAbsent;
    throw std::runtime_error("unknown expectation: " + text);
}

std::string service_state_to_string(ServiceState value) {
    switch (value) {
        case ServiceState::Running: return "Running";
        case ServiceState::Stopped: return "Stopped";
        case ServiceState::Unknown: return "Unknown";
    }
    return "Unknown";
}

ServiceState service_state_from_string(const std::string& text) {
    if (text == "Running") return ServiceState::Running;
    if (text == "Stopped") return ServiceState::Stopped;
    if (text == "Unknown") return ServiceState::Unknown;
    throw std::runtime_error("unknown service state: " + text);
}

std::string registry_match_to_string(RegistryMatch value) {
    switch (value) {
        case RegistryMatch::Exists:   return "Exists";
        case RegistryMatch::Equals:   return "Equals";
        case RegistryMatch::Contains: return "Contains";
    }
    return "Exists";
}

/// Stable wire names, deliberately not core::to_string()'s display strings:
/// "at least" and "No link" are for humans and are free to be reworded, while
/// anything written into a saved bundle has to keep parsing next release.
std::string comparison_to_string(Comparison value) {
    switch (value) {
        case Comparison::AtLeast: return "AtLeast";
        case Comparison::Exactly: return "Exactly";
        case Comparison::AtMost:  return "AtMost";
    }
    return "AtLeast";
}

Comparison comparison_from_string(const std::string& text) {
    if (text == "AtLeast") return Comparison::AtLeast;
    if (text == "Exactly") return Comparison::Exactly;
    if (text == "AtMost")  return Comparison::AtMost;
    throw std::runtime_error("unknown comparison: " + text);
}

// Wire names, deliberately not to_string(DdsMatch)'s display strings ("equal
// to", "at least"): those are free to be reworded, while anything in a saved
// bundle has to keep parsing.
std::string dds_match_to_string(DdsMatch value) {
    switch (value) {
        case DdsMatch::Equals:   return "Equals";
        case DdsMatch::Contains: return "Contains";
        case DdsMatch::AtLeast:  return "AtLeast";
        case DdsMatch::AtMost:   return "AtMost";
    }
    return "Equals";
}

DdsMatch dds_match_from_string(const std::string& text) {
    if (text == "Equals")   return DdsMatch::Equals;
    if (text == "Contains") return DdsMatch::Contains;
    if (text == "AtLeast")  return DdsMatch::AtLeast;
    if (text == "AtMost")   return DdsMatch::AtMost;
    throw std::runtime_error("unknown DDS match: " + text);
}

std::string link_state_to_string(LinkState value) {
    switch (value) {
        case LinkState::Unknown:      return "Unknown";
        case LinkState::Connected:    return "Connected";
        case LinkState::NoMedia:      return "NoMedia";
        case LinkState::Disconnected: return "Disconnected";
        case LinkState::Connecting:   return "Connecting";
        case LinkState::Disabled:     return "Disabled";
        case LinkState::Faulted:      return "Faulted";
    }
    return "Unknown";
}

LinkState link_state_from_string(const std::string& text) {
    if (text == "Unknown")      return LinkState::Unknown;
    if (text == "Connected")    return LinkState::Connected;
    if (text == "NoMedia")      return LinkState::NoMedia;
    if (text == "Disconnected") return LinkState::Disconnected;
    if (text == "Connecting")   return LinkState::Connecting;
    if (text == "Disabled")     return LinkState::Disabled;
    if (text == "Faulted")      return LinkState::Faulted;
    throw std::runtime_error("unknown link state: " + text);
}

RegistryMatch registry_match_from_string(const std::string& text) {
    if (text == "Exists")   return RegistryMatch::Exists;
    if (text == "Equals")   return RegistryMatch::Equals;
    if (text == "Contains") return RegistryMatch::Contains;
    throw std::runtime_error("unknown registry match: " + text);
}

}  // namespace

void to_json(nlohmann::json& j, const Version& value) { j = to_string(value); }

void from_json(const nlohmann::json& j, Version& value) {
    const auto parsed = parse_version(j.get<std::string>());
    if (!parsed) {
        throw std::runtime_error("malformed version: " + j.get<std::string>());
    }
    value = *parsed;
}

void to_json(nlohmann::json& j, const VersionConstraint& value) {
    j = nlohmann::json{{"op", to_string(value.op)}, {"value", value.value}};
}

void from_json(const nlohmann::json& j, VersionConstraint& value) {
    const auto op = parse_comparison_op(j.at("op").get<std::string>());
    if (!op) {
        throw std::runtime_error("unknown comparison operator");
    }
    value.op = *op;
    j.at("value").get_to(value.value);
}

void to_json(nlohmann::json& j, const Rule& value) {
    nlohmann::json payload;
    std::visit(
        [&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ProcessRule>) {
                payload = {{"type", "process"}, {"executable", p.executable}};
            } else if constexpr (std::is_same_v<T, ServiceRule>) {
                payload = {{"type", "service"}, {"service_name", p.service_name}};
                if (p.expected_state) {
                    payload["expected_state"] = service_state_to_string(*p.expected_state);
                }
            } else if constexpr (std::is_same_v<T, RegistryRule>) {
                payload = {{"type", "registry"},
                           {"hive", to_string(p.hive)},
                           {"key_path", p.key_path},
                           {"value_name", p.value_name},
                           {"match", registry_match_to_string(p.match)},
                           {"expected_value", p.expected_value}};
            } else if constexpr (std::is_same_v<T, AdapterCountRule>) {
                payload = {{"type", "adapter_count"},
                           {"comparison", comparison_to_string(p.comparison)},
                           {"count", p.count}};
            } else if constexpr (std::is_same_v<T, AdapterStateRule>) {
                payload = {{"type", "adapter_state"},
                           {"adapter_name", p.adapter_name},
                           {"expected", link_state_to_string(p.expected)}};
            } else if constexpr (std::is_same_v<T, DdsTopicRule>) {
                payload = {{"type", "dds_topic"},
                           {"domain_id", p.domain_id},
                           {"topic_name", p.topic_name}};
            } else {
                payload = {{"type", "dds_value"},
                           {"domain_id", p.domain_id},
                           {"topic_name", p.topic_name},
                           {"path", p.path},
                           {"match", dds_match_to_string(p.match)},
                           {"expected_value", p.expected_value}};
            }
        },
        value.payload);

    j = nlohmann::json{{"id", value.id},
                       {"description", value.description},
                       {"expectation", presence_to_string(value.expectation)},
                       {"payload", payload}};
    if (value.version) {
        j["version"] = *value.version;
    }
}

void from_json(const nlohmann::json& j, Rule& value) {
    j.at("id").get_to(value.id);
    j.at("description").get_to(value.description);
    value.expectation = presence_from_string(j.at("expectation").get<std::string>());

    const nlohmann::json& payload = j.at("payload");
    const std::string type = payload.at("type").get<std::string>();
    if (type == "process") {
        value.payload = ProcessRule{payload.at("executable").get<std::string>()};
    } else if (type == "service") {
        ServiceRule rule;
        payload.at("service_name").get_to(rule.service_name);
        if (payload.contains("expected_state")) {
            rule.expected_state =
                service_state_from_string(payload.at("expected_state").get<std::string>());
        }
        value.payload = rule;
    } else if (type == "registry") {
        RegistryRule rule;
        const auto hive = parse_registry_hive(payload.at("hive").get<std::string>());
        if (!hive) {
            throw std::runtime_error("unknown registry hive");
        }
        rule.hive = *hive;
        payload.at("key_path").get_to(rule.key_path);
        payload.at("value_name").get_to(rule.value_name);
        rule.match = registry_match_from_string(payload.at("match").get<std::string>());
        payload.at("expected_value").get_to(rule.expected_value);
        value.payload = rule;
    } else if (type == "adapter_count") {
        AdapterCountRule rule;
        rule.comparison = comparison_from_string(payload.at("comparison").get<std::string>());
        payload.at("count").get_to(rule.count);
        value.payload = rule;
    } else if (type == "adapter_state") {
        AdapterStateRule rule;
        payload.at("adapter_name").get_to(rule.adapter_name);
        rule.expected = link_state_from_string(payload.at("expected").get<std::string>());
        value.payload = rule;
    } else if (type == "dds_topic") {
        DdsTopicRule rule;
        payload.at("domain_id").get_to(rule.domain_id);
        payload.at("topic_name").get_to(rule.topic_name);
        value.payload = rule;
    } else if (type == "dds_value") {
        DdsValueRule rule;
        payload.at("domain_id").get_to(rule.domain_id);
        payload.at("topic_name").get_to(rule.topic_name);
        payload.at("path").get_to(rule.path);
        rule.match = dds_match_from_string(payload.at("match").get<std::string>());
        payload.at("expected_value").get_to(rule.expected_value);
        value.payload = rule;
    } else {
        throw std::runtime_error("unknown rule payload type: " + type);
    }

    value.version.reset();
    if (j.contains("version")) {
        value.version = j.at("version").get<VersionConstraint>();
    }
}

void to_json(nlohmann::json& j, const Template& value) {
    j = nlohmann::json{{"name", value.name}, {"rules", value.rules}};
}

void from_json(const nlohmann::json& j, Template& value) {
    j.at("name").get_to(value.name);
    j.at("rules").get_to(value.rules);
}

void to_json(nlohmann::json& j, const TemplateBundle& value) {
    j = nlohmann::json{{"revision", value.revision},
                       {"hash", value.hash},
                       {"baseline", value.baseline},
                       {"templates", value.templates},
                       {"assignments", value.assignments}};
}

void from_json(const nlohmann::json& j, TemplateBundle& value) {
    j.at("revision").get_to(value.revision);
    j.at("hash").get_to(value.hash);
    j.at("baseline").get_to(value.baseline);
    j.at("templates").get_to(value.templates);
    j.at("assignments").get_to(value.assignments);
}

std::string serialise_bundle(const TemplateBundle& bundle) {
    return nlohmann::json(bundle).dump(2);
}

std::expected<TemplateBundle, std::string> parse_bundle(std::string_view text) {
    try {
        const nlohmann::json parsed = nlohmann::json::parse(text);
        return parsed.get<TemplateBundle>();
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
}

std::string content_hash(const TemplateBundle& bundle) {
    TemplateBundle content = bundle;
    content.revision = 0;
    content.hash.clear();

    // Ordered dump: nlohmann's default object type sorts keys, and templates and
    // assignments are already stored in a deterministic order.
    const std::string canonical = nlohmann::json(content).dump();

    // FNV-1a 64. Deliberately not std::hash, which is implementation-defined and
    // would give different results across platforms and standard libraries — this
    // value is persisted to disk and compared after restart.
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : canonical) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = "0123456789abcdef"[hash & 0xfu];
        hash >>= 4;
    }
    return out;
}

}  // namespace lm::core
