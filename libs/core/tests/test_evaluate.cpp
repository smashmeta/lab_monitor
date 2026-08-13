#include <gtest/gtest.h>

#include <utility>

#include "lm/core/compliance.hpp"

using namespace lm::core;

namespace {

Capabilities all_capabilities() {
    return Capabilities{}
        .add(Capability::Resources)
        .add(Capability::Processes)
        .add(Capability::Services)
        .add(Capability::Registry);
}

TemplateBundle bundle_with(Rule rule) {
    TemplateBundle bundle;
    bundle.revision = 3;
    bundle.baseline.name = "baseline";
    bundle.baseline.rules.push_back(std::move(rule));
    return bundle;
}

Rule process_rule(Presence expectation, std::optional<VersionConstraint> version = std::nullopt) {
    Rule rule;
    rule.id = "p1";
    rule.expectation = expectation;
    rule.payload = ProcessRule{"antivirus.exe"};
    rule.version = std::move(version);
    return rule;
}

Rule service_rule(Presence expectation, std::optional<ServiceState> state = std::nullopt) {
    Rule rule;
    rule.id = "s1";
    rule.expectation = expectation;
    rule.payload = ServiceRule{"spooler", state};
    return rule;
}

Rule registry_rule(Presence expectation, RegistryMatch match, std::string expected) {
    Rule rule;
    rule.id = "g1";
    rule.expectation = expectation;
    rule.payload = RegistryRule{RegistryHive::LocalMachine, "SOFTWARE\\Acme",
                                "Version", match, std::move(expected)};
    return rule;
}

HostFacts facts_with_process(std::string exe, std::optional<Version> version = std::nullopt) {
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.processes.push_back(ProcessInfo{std::move(exe), std::move(version)});
    return facts;
}

CheckStatus status_of(const ComplianceReport& report) {
    EXPECT_EQ(report.results.size(), 1u);
    return report.results.front().status;
}

}  // namespace

// --- capability gating -----------------------------------------------------

TEST(Evaluate, RegistryRuleIsNotApplicableWithoutTheCapability) {
    const Capabilities linux_caps =
        Capabilities{}.add(Capability::Resources).add(Capability::Processes).add(Capability::Services);
    const auto report = evaluate(bundle_with(registry_rule(Presence::MustBePresent,
                                                           RegistryMatch::Exists, "")),
                                 HostFacts{}, linux_caps);
    EXPECT_EQ(status_of(report), CheckStatus::NotApplicable);
}

TEST(Evaluate, NotApplicableNamesTheMissingCapabilityRatherThanBlamingTheOs) {
    // "not supported on this platform" is wrong whenever the capability is
    // absent because the probe is unimplemented rather than because the OS
    // cannot serve it -- which is the case for registry checks on Windows
    // while IRegistryProbe is stubbed. The message must say which capability
    // is missing and must not assert an OS limitation.
    const Capabilities without_registry =
        Capabilities{}.add(Capability::Resources).add(Capability::Processes).add(Capability::Services);
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")), HostFacts{},
        without_registry);

    ASSERT_EQ(report.results.size(), 1u);
    const std::string& observed = report.results.front().observed;
    EXPECT_NE(observed.find("Registry"), std::string::npos) << "observed was: " << observed;
    EXPECT_EQ(observed.find("platform"), std::string::npos)
        << "must not claim an OS limitation; observed was: " << observed;
}

TEST(ToStringCapability, NamesEveryCapability) {
    EXPECT_EQ(to_string(Capability::Resources), "Resources");
    EXPECT_EQ(to_string(Capability::Processes), "Processes");
    EXPECT_EQ(to_string(Capability::Services), "Services");
    EXPECT_EQ(to_string(Capability::Registry), "Registry");
}

TEST(Evaluate, NotApplicableDoesNotMakeAHostNonCompliant) {
    const Capabilities linux_caps = Capabilities{}.add(Capability::Processes);
    const auto report = evaluate(bundle_with(registry_rule(Presence::MustBePresent,
                                                           RegistryMatch::Exists, "")),
                                 HostFacts{}, linux_caps);
    EXPECT_TRUE(is_compliant(report));
}

// --- report metadata -------------------------------------------------------

TEST(Evaluate, CarriesHostIdAndAppliedRevision) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                 facts_with_process("antivirus.exe"), all_capabilities());
    EXPECT_EQ(report.host_id, "PC-001");
    EXPECT_EQ(report.applied_revision, 3u);
}

TEST(Evaluate, EmptyBundleProducesEmptyCompliantReport) {
    const auto report = evaluate(TemplateBundle{}, facts_with_process("anything.exe"),
                                 all_capabilities());
    EXPECT_TRUE(report.results.empty());
    EXPECT_TRUE(is_compliant(report));
}

// --- process rules ---------------------------------------------------------

TEST(Evaluate, ProcessMustBePresentPassesWhenRunning) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                 facts_with_process("antivirus.exe"), all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ProcessNameMatchIsCaseInsensitive) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                 facts_with_process("AntiVirus.EXE"), all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ProcessMustBePresentFailsWhenAbsent) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                 HostFacts{}, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
    EXPECT_FALSE(is_compliant(report));
}

TEST(Evaluate, ProcessMustBeAbsentPassesWhenAbsent) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBeAbsent)),
                                 HostFacts{}, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ProcessMustBeAbsentFailsWhenRunning) {
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBeAbsent)),
                                 facts_with_process("antivirus.exe"), all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
}

TEST(Evaluate, ProcessVersionConstraintSatisfied) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("2.0")};
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent, constraint)),
                                 facts_with_process("antivirus.exe", parse_version("2.1")),
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ProcessVersionConstraintViolatedFails) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("3.0")};
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent, constraint)),
                                 facts_with_process("antivirus.exe", parse_version("2.1")),
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
    EXPECT_NE(report.results.front().observed.find("2.1"), std::string::npos);
}

TEST(Evaluate, ProcessVersionUnreadableIsAnErrorNotAFailure) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("3.0")};
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBePresent, constraint)),
                                 facts_with_process("antivirus.exe", std::nullopt),
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Error);
    EXPECT_FALSE(report.results.front().message.empty());
}

TEST(Evaluate, MustBeAbsentIgnoresVersionConstraint) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("3.0")};
    const auto report = evaluate(bundle_with(process_rule(Presence::MustBeAbsent, constraint)),
                                 HostFacts{}, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

// --- service rules ---------------------------------------------------------

TEST(Evaluate, ServiceMustBePresentPassesWhenInstalled) {
    HostFacts facts;
    facts.services.push_back(ServiceInfo{"Spooler", ServiceState::Running});
    const auto report = evaluate(bundle_with(service_rule(Presence::MustBePresent)), facts,
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ServiceExpectedStateMismatchFails) {
    HostFacts facts;
    facts.services.push_back(ServiceInfo{"spooler", ServiceState::Stopped});
    const auto report = evaluate(
        bundle_with(service_rule(Presence::MustBePresent, ServiceState::Running)), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
    EXPECT_NE(report.results.front().observed.find("Stopped"), std::string::npos);
}

TEST(Evaluate, ServiceExpectedStateMatchPasses) {
    HostFacts facts;
    facts.services.push_back(ServiceInfo{"spooler", ServiceState::Running});
    const auto report = evaluate(
        bundle_with(service_rule(Presence::MustBePresent, ServiceState::Running)), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, ServiceMustBeAbsentFailsWhenInstalled) {
    HostFacts facts;
    facts.services.push_back(ServiceInfo{"spooler", ServiceState::Stopped});
    const auto report = evaluate(bundle_with(service_rule(Presence::MustBeAbsent)), facts,
                                 all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Fail);
}

// --- registry rules --------------------------------------------------------

TEST(Evaluate, RegistryUnprobedKeyIsAnError) {
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")),
        HostFacts{}, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Error);
}

TEST(Evaluate, RegistryReadFailureSurfacesTheOsMessage) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] =
        RegistryValue{false, "", "ERROR_ACCESS_DENIED"};
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Error);
    EXPECT_EQ(report.results.front().message, "ERROR_ACCESS_DENIED");
}

TEST(Evaluate, RegistryExistsPasses) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] = RegistryValue{true, "4.2", ""};
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, RegistryEqualsComparesExactValue) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] = RegistryValue{true, "4.2", ""};

    const auto match = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Equals, "4.2")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(match), CheckStatus::Pass);

    const auto mismatch = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Equals, "4.3")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(mismatch), CheckStatus::Fail);
}

TEST(Evaluate, RegistryContainsMatchesSubstring) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] = RegistryValue{true, "4.2-beta", ""};
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Contains, "beta")),
        facts, all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

TEST(Evaluate, RegistryMustBeAbsentPassesWhenValueMissing) {
    HostFacts facts;
    facts.registry["HKLM\\SOFTWARE\\Acme\\\\Version"] = RegistryValue{false, "", ""};
    const auto report = evaluate(
        bundle_with(registry_rule(Presence::MustBeAbsent, RegistryMatch::Exists, "")), facts,
        all_capabilities());
    EXPECT_EQ(status_of(report), CheckStatus::Pass);
}

// --- aggregation -----------------------------------------------------------

TEST(CountByStatus, TalliesEachStatus) {
    TemplateBundle bundle;
    bundle.baseline.rules = {process_rule(Presence::MustBePresent),
                             registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")};
    bundle.baseline.rules[1].id = "g1";

    const Capabilities caps = Capabilities{}.add(Capability::Processes);
    const auto report = evaluate(bundle, HostFacts{}, caps);

    EXPECT_EQ(count_by_status(report, CheckStatus::Fail), 1u);
    EXPECT_EQ(count_by_status(report, CheckStatus::NotApplicable), 1u);
    EXPECT_EQ(count_by_status(report, CheckStatus::Pass), 0u);
}
