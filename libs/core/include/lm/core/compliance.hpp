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

}  // namespace lm::core
