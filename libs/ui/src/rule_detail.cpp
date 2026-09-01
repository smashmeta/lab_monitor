#include "lm/ui/rule_detail.hpp"

#include <type_traits>
#include <variant>

namespace lm::ui {
namespace {

QString to_qstring(const std::string& text) { return QString::fromStdString(text); }

QString service_state_name(lm::core::ServiceState state) {
    switch (state) {
        case lm::core::ServiceState::Running: return QStringLiteral("Running");
        case lm::core::ServiceState::Stopped: return QStringLiteral("Stopped");
        case lm::core::ServiceState::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

QString registry_match_name(lm::core::RegistryMatch match) {
    switch (match) {
        case lm::core::RegistryMatch::Exists:   return QStringLiteral("exists");
        case lm::core::RegistryMatch::Equals:   return QStringLiteral("equals");
        case lm::core::RegistryMatch::Contains: return QStringLiteral("contains");
    }
    return QStringLiteral("exists");
}

/// Readable registry path. Deliberately not core::registry_key(), whose doubled
/// separator is an internal lookup key rather than something to show a person.
QString registry_target(const lm::core::RegistryRule& payload) {
    const QString value = payload.value_name.empty() ? QStringLiteral("(Default)")
                                                     : to_qstring(payload.value_name);
    return to_qstring(lm::core::to_string(payload.hive)) + QStringLiteral("\\") +
           to_qstring(payload.key_path) + QStringLiteral("\\") + value;
}

}  // namespace

QString RuleDetail::tooltip() const {
    QString text = label;
    text += QStringLiteral("\n\n");
    text += QStringLiteral("Kind:\t%1\n").arg(kind);
    text += QStringLiteral("Target:\t%1\n").arg(target);
    text += QStringLiteral("Expect:\t%1").arg(expectation);
    if (!constraint.isEmpty()) {
        text += QStringLiteral("\nAlso:\t%1").arg(constraint);
    }
    // Kept for support conversations, where an operator needs to name the exact
    // rule -- just not as the row label, which is what the description is for.
    text += QStringLiteral("\n\nRule id: %1").arg(id);
    return text;
}

RuleDetail describe(const lm::core::Rule& rule) {
    RuleDetail detail;
    detail.id = to_qstring(rule.id);
    detail.expectation = rule.expectation == lm::core::Presence::MustBePresent
                             ? QStringLiteral("Must be present")
                             : QStringLiteral("Must be absent");

    std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, lm::core::ProcessRule>) {
                detail.kind = QStringLiteral("Application");
                detail.target = to_qstring(payload.executable);
                if (rule.version) {
                    detail.constraint =
                        QStringLiteral("version %1 %2")
                            .arg(to_qstring(lm::core::to_string(rule.version->op)),
                                 to_qstring(lm::core::to_string(rule.version->value)));
                }
            } else if constexpr (std::is_same_v<T, lm::core::ServiceRule>) {
                detail.kind = QStringLiteral("Service");
                detail.target = to_qstring(payload.service_name);
                if (payload.expected_state) {
                    detail.constraint =
                        QStringLiteral("state %1").arg(service_state_name(*payload.expected_state));
                }
            } else if constexpr (std::is_same_v<T, lm::core::RegistryRule>) {
                detail.kind = QStringLiteral("Registry");
                detail.target = registry_target(payload);
                detail.constraint = registry_match_name(payload.match);
                if (payload.match != lm::core::RegistryMatch::Exists) {
                    detail.constraint += QStringLiteral(" \"%1\"").arg(to_qstring(payload.expected_value));
                }
            } else if constexpr (std::is_same_v<T, lm::core::AdapterCountRule>) {
                detail.kind = QStringLiteral("Network");
                detail.target = QStringLiteral("connected adapters");
                detail.constraint = QStringLiteral("%1 %2")
                                        .arg(to_qstring(lm::core::to_string(payload.comparison)))
                                        .arg(payload.count);
                // The comparison already says the direction, so showing
                // "Must be present" beside it would only invite the reader to
                // wonder which one wins.
                detail.expectation = detail.constraint;
            } else if constexpr (std::is_same_v<T, lm::core::AdapterStateRule>) {
                detail.kind = QStringLiteral("Network");
                detail.target = to_qstring(payload.adapter_name);
                detail.constraint =
                    QStringLiteral("link %1").arg(to_qstring(lm::core::to_string(payload.expected)));
            } else if constexpr (std::is_same_v<T, lm::core::DdsTopicRule>) {
                detail.kind = QStringLiteral("DDS");
                // Domain and topic together: the same topic name on two domains
                // is two different things, and the domain is the half a reader
                // will not guess.
                detail.target = QStringLiteral("%1 on domain %2")
                                    .arg(to_qstring(payload.topic_name))
                                    .arg(payload.domain_id);
                detail.constraint = QStringLiteral("the topic is published");
            } else {
                detail.kind = QStringLiteral("DDS");
                // "any" for a wildcard path, because the rule passes on the
                // first element that matches and reading it as though one
                // particular element were meant would be wrong in exactly the
                // situation the wildcard exists for.
                const QString path = to_qstring(payload.path);
                const QString addressed = path.contains(QStringLiteral("[*]"))
                                              ? QStringLiteral("any %1").arg(path)
                                              : path;
                detail.target = QStringLiteral("%1.%2 on domain %3")
                                    .arg(to_qstring(payload.topic_name))
                                    .arg(addressed)
                                    .arg(payload.domain_id);
                detail.constraint = QStringLiteral("%1 %2")
                                        .arg(to_qstring(lm::core::to_string(payload.match)))
                                        .arg(to_qstring(payload.expected_value));
                // As with the adapter count, the match carries the direction,
                // so a separate "Must be present" would only be a second
                // answer to a question already settled.
                detail.expectation = detail.constraint;
            }
        },
        rule.payload);

    detail.label = to_qstring(rule.description);
    if (detail.label.trimmed().isEmpty()) {
        // An unnamed rule still has to be identifiable at a glance; the target
        // is more use than the opaque id it replaces.
        detail.label = QStringLiteral("%1: %2").arg(detail.kind, detail.target);
    }
    return detail;
}

}  // namespace lm::ui
