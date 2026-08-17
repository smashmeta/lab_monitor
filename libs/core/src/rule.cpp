#include "lm/core/rule.hpp"

#include <type_traits>

namespace lm::core {

RuleKind kind_of(const Rule& rule) {
    return std::visit(
        [](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, ProcessRule>) {
                return RuleKind::Process;
            } else if constexpr (std::is_same_v<T, ServiceRule>) {
                return RuleKind::Service;
            } else if constexpr (std::is_same_v<T, RegistryRule>) {
                return RuleKind::Registry;
            } else {
                // Both adapter payloads: the kind is the capability they need,
                // and they need the same one.
                return RuleKind::Network;
            }
        },
        rule.payload);
}

std::string to_string(RegistryHive hive) {
    switch (hive) {
        case RegistryHive::LocalMachine: return "HKLM";
        case RegistryHive::CurrentUser:  return "HKCU";
        case RegistryHive::ClassesRoot:  return "HKCR";
        case RegistryHive::Users:        return "HKU";
    }
    return "HKLM";
}

std::optional<RegistryHive> parse_registry_hive(std::string_view text) {
    if (text == "HKLM") return RegistryHive::LocalMachine;
    if (text == "HKCU") return RegistryHive::CurrentUser;
    if (text == "HKCR") return RegistryHive::ClassesRoot;
    if (text == "HKU")  return RegistryHive::Users;
    return std::nullopt;
}

std::string registry_key(const RegistryRule& rule) {
    return to_string(rule.hive) + "\\" + rule.key_path + "\\\\" + rule.value_name;
}

}  // namespace lm::core
