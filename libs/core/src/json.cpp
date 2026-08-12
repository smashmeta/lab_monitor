#include "lm/core/json.hpp"

#include <cstdint>
#include <stdexcept>

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
            } else {
                payload = {{"type", "registry"},
                           {"hive", to_string(p.hive)},
                           {"key_path", p.key_path},
                           {"value_name", p.value_name},
                           {"match", registry_match_to_string(p.match)},
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
    std::uint64_t hash = 1469598103934665603ull;
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
