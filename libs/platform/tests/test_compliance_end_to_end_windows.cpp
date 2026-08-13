#include <gtest/gtest.h>

#include <algorithm>

#include "lm/core/compliance.hpp"
#include "lm/platform/probes.hpp"

using namespace lm::core;
using namespace lm::platform;

namespace {

/// Runs a single rule through the real platform probes and the real evaluator,
/// exactly as the client does: collect facts, then evaluate against them.
CheckResult check(Rule rule) {
    TemplateBundle bundle;
    bundle.revision = 1;
    bundle.baseline.name = "baseline";
    bundle.baseline.rules.push_back(std::move(rule));

    HostProbes probes{local_host_name(), make_platform_probes(), platform_capabilities()};
    const HostFacts facts = probes.collect(bundle);
    const ComplianceReport report = evaluate(bundle, facts, probes.capabilities());

    EXPECT_EQ(report.results.size(), 1u);
    return report.results.empty() ? CheckResult{} : report.results.front();
}

Rule process_rule(std::string executable, Presence expectation) {
    Rule rule;
    rule.id = "p1";
    rule.expectation = expectation;
    rule.payload = ProcessRule{std::move(executable)};
    return rule;
}

Rule registry_rule(std::string key_path, std::string value_name, RegistryMatch match,
                   std::string expected, Presence expectation) {
    Rule rule;
    rule.id = "g1";
    rule.expectation = expectation;
    rule.payload = RegistryRule{RegistryHive::LocalMachine, std::move(key_path),
                                std::move(value_name), match, std::move(expected)};
    return rule;
}

}  // namespace

// These are the assertions that would have failed before the Windows process
// and registry probes were wired into make_platform_probes(): every one of them
// returned NotApplicable regardless of what was actually on the machine.

TEST(WindowsComplianceEndToEnd, ARunningProcessThatMustBePresentPasses) {
    const CheckResult result = check(process_rule("lm_platform_tests.exe", Presence::MustBePresent));

    EXPECT_EQ(result.status, CheckStatus::Pass) << "observed: " << result.observed;
}

TEST(WindowsComplianceEndToEnd, AnAbsentProcessThatMustBePresentFails) {
    const CheckResult result =
        check(process_rule("lab_monitor_no_such_process_7f3a.exe", Presence::MustBePresent));

    EXPECT_EQ(result.status, CheckStatus::Fail) << "observed: " << result.observed;
}

TEST(WindowsComplianceEndToEnd, AnAbsentProcessThatMustBeAbsentPasses) {
    const CheckResult result =
        check(process_rule("lab_monitor_no_such_process_7f3a.exe", Presence::MustBeAbsent));

    EXPECT_EQ(result.status, CheckStatus::Pass) << "observed: " << result.observed;
}

TEST(WindowsComplianceEndToEnd, AnExistingRegistryValueThatMustBePresentPasses) {
    const CheckResult result =
        check(registry_rule("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName",
                            RegistryMatch::Exists, "", Presence::MustBePresent));

    EXPECT_EQ(result.status, CheckStatus::Pass) << "observed: " << result.observed;
    EXPECT_NE(result.observed.find("Windows"), std::string::npos) << result.observed;
}

TEST(WindowsComplianceEndToEnd, ARegistryValueComparedWithEqualsFailsOnAMismatch) {
    const CheckResult result = check(
        registry_rule("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentMajorVersionNumber",
                      RegistryMatch::Equals, "999", Presence::MustBePresent));

    // The value exists but does not equal 999, so this is a genuine Fail --
    // not NotApplicable, and not Error.
    EXPECT_EQ(result.status, CheckStatus::Fail) << "observed: " << result.observed;
}

TEST(WindowsComplianceEndToEnd, AMissingRegistryValueThatMustBeAbsentPasses) {
    const CheckResult result =
        check(registry_rule("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                            "LabMonitorNoSuchValue_7f3a", RegistryMatch::Exists, "",
                            Presence::MustBeAbsent));

    EXPECT_EQ(result.status, CheckStatus::Pass) << "observed: " << result.observed;
}

TEST(WindowsComplianceEndToEnd, ServiceRulesStillReportNotApplicable) {
    // Services remain deliberately stubbed, and the message must say so without
    // blaming the operating system.
    Rule rule;
    rule.id = "s1";
    rule.expectation = Presence::MustBePresent;
    rule.payload = ServiceRule{"Spooler", std::nullopt};

    const CheckResult result = check(std::move(rule));

    EXPECT_EQ(result.status, CheckStatus::NotApplicable);
    EXPECT_NE(result.observed.find("Services"), std::string::npos) << result.observed;
    EXPECT_EQ(result.observed.find("platform"), std::string::npos) << result.observed;
}
