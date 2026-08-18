#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lm/core/host_facts.hpp"
#include "lm/core/template_bundle.hpp"

namespace lm::core {

struct CheckResult {
    RuleId rule_id;
    CheckStatus status = CheckStatus::NotApplicable;
    /// Human-readable description of what was actually observed.
    std::string observed;
    /// Populated only when status == CheckStatus::Error.
    std::string message;
    friend bool operator==(const CheckResult&, const CheckResult&) = default;
};

struct ComplianceReport {
    HostId host_id;
    std::uint64_t applied_revision = 0;
    std::vector<CheckResult> results;
    friend bool operator==(const ComplianceReport&, const ComplianceReport&) = default;
};

/// Evaluates every rule applying to facts.host_id. Pure: no I/O, no clock, no
/// global state. Rules whose required capability is absent from caps yield
/// CheckStatus::NotApplicable.
[[nodiscard]] ComplianceReport evaluate(const TemplateBundle& bundle, const HostFacts& facts,
                                        Capabilities caps);

[[nodiscard]] std::size_t count_by_status(const ComplianceReport& report, CheckStatus status);

/// Only CheckStatus::Fail counts against compliance. NotApplicable and Error are
/// reported but do not mark a host as non-compliant.
[[nodiscard]] bool is_compliant(const ComplianceReport& report);

/// One report reduced to counts, for "12 of 15 rules passed".
struct ComplianceSummary {
    std::size_t passed = 0;
    std::size_t failing = 0;
    std::size_t errors = 0;
    std::size_t not_applicable = 0;

    /// The rules this host could actually be judged on.
    ///
    /// NotApplicable is excluded deliberately. A rule the client cannot
    /// evaluate can never pass, so counting it in the denominator would park a
    /// Linux machine at "5 of 10" forever for the crime of having no registry —
    /// and that number would never improve however compliant it became. The
    /// excluded ones stay visible in their own count rather than vanishing.
    [[nodiscard]] std::size_t checked() const { return passed + failing + errors; }
    [[nodiscard]] std::size_t total() const { return checked() + not_applicable; }

    /// passed / checked(), in [0, 1]. A ratio rather than a percentage because
    /// that is how this is reported — "12 / 15", not "80%" — and one
    /// representation is enough. 1.0 when nothing was checked: nothing is
    /// failing, and a host with no applicable rules is not non-compliant.
    [[nodiscard]] double passed_ratio() const;

    friend bool operator==(const ComplianceSummary&, const ComplianceSummary&) = default;
};

[[nodiscard]] ComplianceSummary summarise(const ComplianceReport& report);

}  // namespace lm::core
