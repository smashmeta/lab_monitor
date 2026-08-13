#pragma once

#include <QString>

#include "lm/core/rule.hpp"

/// Display fields for one compliance rule.
///
/// core::CheckResult travels over the wire carrying only a rule id, a status
/// and an observed value -- never the rule's description or payload. The client
/// holds the parsed TemplateBundle locally, so it can recover those here and
/// hand them to the detail window, without widening the wire format or the
/// lm_core contract that the server and every codec depend on.
struct RuleDetail {
    QString id;
    /// What to show in the rule column: the authored description, or a
    /// generated summary when the author left it blank.
    QString label;
    /// "Application", "Service" or "Registry".
    QString kind;
    /// The thing being checked: an executable name, a service name, or a
    /// readable registry path.
    QString target;
    /// "Must be present" or "Must be absent".
    QString expectation;
    /// Any qualifier: a version constraint, an expected service state, or a
    /// registry match. Empty when the rule only checks presence.
    QString constraint;

    /// Multi-line text for the row's hover tooltip.
    [[nodiscard]] QString tooltip() const;
};

/// Builds the display fields for a rule. Pure: no Qt widgets, no I/O.
[[nodiscard]] RuleDetail describe(const lm::core::Rule& rule);
