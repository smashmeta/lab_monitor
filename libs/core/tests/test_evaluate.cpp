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
        .add(Capability::Registry)
        .add(Capability::Network);
}

NetworkAdapter adapter(std::string name, LinkState link) {
    NetworkAdapter entry;
    entry.name = std::move(name);
    entry.id = "{" + entry.name + "}";
    entry.type = AdapterType::Ethernet;
    entry.link = link;
    return entry;
}

Rule count_rule(Comparison comparison, int count) {
    Rule rule;
    rule.id = "n1";
    rule.payload = AdapterCountRule{comparison, count};
    return rule;
}

Rule adapter_state_rule(std::string name, LinkState expected,
                        Presence expectation = Presence::MustBePresent) {
    Rule rule;
    rule.id = "n2";
    rule.expectation = expectation;
    rule.payload = AdapterStateRule{std::move(name), expected};
    return rule;
}

HostFacts facts_with_adapters(std::vector<NetworkAdapter> adapters) {
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.resources.adapters = std::move(adapters);
    return facts;
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

// --- network rules ----------------------------------------------------------

TEST(EvaluateAdapterCount, PassesWhenEnoughAdaptersAreConnected) {
    const ComplianceReport report =
        evaluate(bundle_with(count_rule(Comparison::AtLeast, 2)),
                 facts_with_adapters({adapter("smash-lan", LinkState::Connected),
                                      adapter("smash-wifi", LinkState::Connected),
                                      adapter("Bluetooth", LinkState::NoMedia)}),
                 all_capabilities());

    ASSERT_EQ(report.results.size(), 1u);
    EXPECT_EQ(report.results.front().status, CheckStatus::Pass);
    EXPECT_EQ(report.results.front().observed, "2 of 3 connected");
}

TEST(EvaluateAdapterCount, CountsOnlyFullyConnectedAdapters) {
    // The states that are not Connected are each a different problem, but none
    // of them is carrying traffic -- the same definition the fleet column uses.
    const ComplianceReport report =
        evaluate(bundle_with(count_rule(Comparison::AtLeast, 1)),
                 facts_with_adapters({adapter("a", LinkState::NoMedia),
                                      adapter("b", LinkState::Connecting),
                                      adapter("c", LinkState::Disabled),
                                      adapter("d", LinkState::Faulted)}),
                 all_capabilities());

    EXPECT_EQ(report.results.front().status, CheckStatus::Fail);
    EXPECT_EQ(report.results.front().observed, "0 of 4 connected");
}

TEST(EvaluateAdapterCount, ExactlyRejectsBothTooFewAndTooMany) {
    const auto status_for = [](int connected) {
        std::vector<NetworkAdapter> adapters;
        for (int i = 0; i < connected; ++i) {
            adapters.push_back(adapter("nic" + std::to_string(i), LinkState::Connected));
        }
        return evaluate(bundle_with(count_rule(Comparison::Exactly, 2)),
                        facts_with_adapters(std::move(adapters)), all_capabilities())
            .results.front()
            .status;
    };

    EXPECT_EQ(status_for(1), CheckStatus::Fail);
    EXPECT_EQ(status_for(2), CheckStatus::Pass);
    EXPECT_EQ(status_for(3), CheckStatus::Fail);
}

TEST(EvaluateAdapterCount, AtMostCatchesAnUnexpectedExtraConnection) {
    // The reason AtMost exists: a lab machine quietly bridged onto a second
    // network is exactly what a fleet check should notice.
    const ComplianceReport report =
        evaluate(bundle_with(count_rule(Comparison::AtMost, 1)),
                 facts_with_adapters({adapter("smash-lan", LinkState::Connected),
                                      adapter("tethered-phone", LinkState::Connected)}),
                 all_capabilities());

    EXPECT_EQ(report.results.front().status, CheckStatus::Fail);
}

TEST(EvaluateAdapterCount, PassesWithNoAdaptersWhenNoneAreRequired) {
    const ComplianceReport report = evaluate(bundle_with(count_rule(Comparison::AtMost, 0)),
                                             facts_with_adapters({}), all_capabilities());
    EXPECT_EQ(report.results.front().status, CheckStatus::Pass);
}

TEST(EvaluateAdapterState, PassesWhenTheNamedAdapterIsInTheRequiredState) {
    const ComplianceReport report =
        evaluate(bundle_with(adapter_state_rule("smash-wifi", LinkState::Connected)),
                 facts_with_adapters({adapter("smash-lan", LinkState::NoMedia),
                                      adapter("smash-wifi", LinkState::Connected)}),
                 all_capabilities());

    EXPECT_EQ(report.results.front().status, CheckStatus::Pass);
    EXPECT_EQ(report.results.front().observed, "Up");
}

TEST(EvaluateAdapterState, FailsWhenItIsInADifferentState) {
    const ComplianceReport report =
        evaluate(bundle_with(adapter_state_rule("smash-lan", LinkState::Connected)),
                 facts_with_adapters({adapter("smash-lan", LinkState::NoMedia)}),
                 all_capabilities());

    EXPECT_EQ(report.results.front().status, CheckStatus::Fail);
    EXPECT_EQ(report.results.front().observed, "No link")
        << "the observed state is the whole point: 'not connected' would not say why";
}

TEST(EvaluateAdapterState, CanRequireAStateOtherThanConnected) {
    // "the guest port must stay unplugged" is as reasonable as its opposite.
    const ComplianceReport report =
        evaluate(bundle_with(adapter_state_rule("guest-port", LinkState::NoMedia)),
                 facts_with_adapters({adapter("guest-port", LinkState::NoMedia)}),
                 all_capabilities());

    EXPECT_EQ(report.results.front().status, CheckStatus::Pass);
}

TEST(EvaluateAdapterState, MustBeAbsentInvertsTheMatch) {
    const auto status_for = [](LinkState actual) {
        return evaluate(bundle_with(adapter_state_rule("guest-port", LinkState::Connected,
                                                        Presence::MustBeAbsent)),
                        facts_with_adapters({adapter("guest-port", actual)}), all_capabilities())
            .results.front()
            .status;
    };

    EXPECT_EQ(status_for(LinkState::Connected), CheckStatus::Fail);
    EXPECT_EQ(status_for(LinkState::NoMedia), CheckStatus::Pass);
}

TEST(EvaluateAdapterState, MatchesTheNameCaseInsensitively) {
    // Rules are typed by people; process and service rules match this way too.
    const ComplianceReport report =
        evaluate(bundle_with(adapter_state_rule("SMASH-WIFI", LinkState::Connected)),
                 facts_with_adapters({adapter("smash-wifi", LinkState::Connected)}),
                 all_capabilities());

    EXPECT_EQ(report.results.front().status, CheckStatus::Pass);
}

TEST(EvaluateAdapterState, FailsRatherThanErrorsWhenTheAdapterIsMissing) {
    // A removed or renamed NIC is exactly what a fleet check should catch, so
    // it is a failure of the rule -- not an inability to tell.
    const ComplianceReport report =
        evaluate(bundle_with(adapter_state_rule("smash-lan", LinkState::Connected)),
                 facts_with_adapters({adapter("smash-wifi", LinkState::Connected)}),
                 all_capabilities());

    EXPECT_EQ(report.results.front().status, CheckStatus::Fail);
    EXPECT_EQ(report.results.front().observed, "no adapter named \"smash-lan\"");
}

TEST(EvaluateNetworkRules, AreNotApplicableWithoutTheNetworkCapability) {
    const ComplianceReport report =
        evaluate(bundle_with(count_rule(Comparison::AtLeast, 1)), facts_with_adapters({}),
                 Capabilities{}.add(Capability::Resources));

    ASSERT_EQ(report.results.size(), 1u);
    EXPECT_EQ(report.results.front().status, CheckStatus::NotApplicable);
    EXPECT_NE(report.results.front().observed.find("Network"), std::string::npos);
}

// --- summaries --------------------------------------------------------------

namespace {

ComplianceReport report_with(std::vector<CheckStatus> statuses) {
    ComplianceReport report;
    report.host_id = "PC-001";
    int n = 0;
    for (const CheckStatus status : statuses) {
        report.results.push_back(CheckResult{"r" + std::to_string(n++), status, "", ""});
    }
    return report;
}

}  // namespace

TEST(Summarise, CountsEachStatusSeparately) {
    const ComplianceSummary summary = summarise(report_with(
        {CheckStatus::Pass, CheckStatus::Pass, CheckStatus::Fail, CheckStatus::Error,
         CheckStatus::NotApplicable}));

    EXPECT_EQ(summary.passed, 2u);
    EXPECT_EQ(summary.failing, 1u);
    EXPECT_EQ(summary.errors, 1u);
    EXPECT_EQ(summary.not_applicable, 1u);
    EXPECT_EQ(summary.total(), 5u);
}

TEST(Summarise, ExcludesNotApplicableFromWhatWasChecked) {
    // The whole point of separating them: a rule the client cannot evaluate can
    // never pass, so counting it in the denominator would park a host at a
    // score it can never improve.
    const ComplianceSummary summary = summarise(report_with(
        {CheckStatus::Pass, CheckStatus::Pass, CheckStatus::NotApplicable,
         CheckStatus::NotApplicable}));

    EXPECT_EQ(summary.checked(), 2u);
    EXPECT_EQ(summary.total(), 4u);
    EXPECT_DOUBLE_EQ(summary.passed_ratio(), 1.0)
        << "two of two applicable rules pass; the other two could not be judged";
}

TEST(Summarise, ErrorsCountAgainstTheScoreWithoutBeingFailures) {
    // is_compliant() ignores errors, but "could not check it" is not the same
    // as "passed" and must not be scored as one.
    const ComplianceSummary summary =
        summarise(report_with({CheckStatus::Pass, CheckStatus::Error}));

    EXPECT_EQ(summary.checked(), 2u);
    EXPECT_DOUBLE_EQ(summary.passed_ratio(), 0.5);
}

TEST(Summarise, AnEmptyReportIsFullyCompliant) {
    const ComplianceSummary summary = summarise(ComplianceReport{});
    EXPECT_EQ(summary.checked(), 0u);
    EXPECT_DOUBLE_EQ(summary.passed_ratio(), 1.0)
        << "a host with no applicable rules is not failing any";
}

TEST(Summarise, AllNotApplicableIsAlsoFullyCompliant) {
    const ComplianceSummary summary =
        summarise(report_with({CheckStatus::NotApplicable, CheckStatus::NotApplicable}));
    EXPECT_EQ(summary.checked(), 0u);
    EXPECT_DOUBLE_EQ(summary.passed_ratio(), 1.0);
}

TEST(Summarise, AgreesWithIsCompliant) {
    EXPECT_TRUE(is_compliant(report_with({CheckStatus::Pass, CheckStatus::Error})));
    EXPECT_EQ(summarise(report_with({CheckStatus::Pass, CheckStatus::Error})).failing, 0u);

    EXPECT_FALSE(is_compliant(report_with({CheckStatus::Fail})));
    EXPECT_EQ(summarise(report_with({CheckStatus::Fail})).failing, 1u);
}

// --- every failure says what was expected ------------------------------------
//
// A failure that only states the observation ("2 of 4 connected") makes the
// reader work out which rule it violates. On a wall display the description is
// the only other text on the row, so the result has to carry the expectation.

namespace {

/// By value, not by reference: every caller passes the temporary that
/// evaluate() returns, and a reference into it would dangle the moment the
/// full expression ends.
CheckResult only_result(const ComplianceReport& report) {
    EXPECT_EQ(report.results.size(), 1u);
    return report.results.empty() ? CheckResult{} : report.results.front();
}

}  // namespace

TEST(FailureMessages, AdapterCountNamesTheThresholdItWanted) {
    const CheckResult result = only_result(evaluate(
        bundle_with(count_rule(Comparison::AtLeast, 3)),
        facts_with_adapters({adapter("a", LinkState::Connected), adapter("b", LinkState::NoMedia)}),
        all_capabilities()));

    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_EQ(result.observed, "1 of 2 connected");
    EXPECT_EQ(result.message, "expected at least 3 connected");
}

TEST(FailureMessages, AdapterCountPhrasesEveryComparison) {
    const auto message_for = [](Comparison comparison, int count) {
        return only_result(evaluate(bundle_with(count_rule(comparison, count)),
                                     facts_with_adapters({adapter("a", LinkState::Connected)}),
                                     all_capabilities()))
            .message;
    };

    EXPECT_EQ(message_for(Comparison::AtLeast, 2), "expected at least 2 connected");
    EXPECT_EQ(message_for(Comparison::Exactly, 2), "expected exactly 2 connected");
    EXPECT_EQ(message_for(Comparison::AtMost, 0), "expected at most 0 connected");
}

TEST(FailureMessages, AdapterStateNamesTheAdapterAndTheState) {
    const CheckResult result =
        only_result(evaluate(bundle_with(adapter_state_rule("smash-lan", LinkState::Connected)),
                             facts_with_adapters({adapter("smash-lan", LinkState::NoMedia)}),
                             all_capabilities()));

    EXPECT_EQ(result.observed, "No link");
    EXPECT_EQ(result.message, "expected \"smash-lan\" to be Up");
}

TEST(FailureMessages, AdapterStateReadsTheRightWayRoundForMustBeAbsent) {
    // "expected it to be Up" on a rule that wanted it down would be worse than
    // saying nothing.
    const CheckResult result = only_result(evaluate(
        bundle_with(adapter_state_rule("guest", LinkState::Connected, Presence::MustBeAbsent)),
        facts_with_adapters({adapter("guest", LinkState::Connected)}), all_capabilities()));

    EXPECT_EQ(result.message, "expected \"guest\" not to be Up");
}

TEST(FailureMessages, AMissingAdapterStillSaysWhatWasWanted) {
    const CheckResult result =
        only_result(evaluate(bundle_with(adapter_state_rule("smash-lan", LinkState::Connected)),
                             facts_with_adapters({}), all_capabilities()));

    EXPECT_EQ(result.observed, "no adapter named \"smash-lan\"");
    EXPECT_EQ(result.message, "expected \"smash-lan\" to be Up");
}

TEST(FailureMessages, MissingProcessNamesTheExecutable) {
    const CheckResult result = only_result(evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                               HostFacts{}, all_capabilities()));
    EXPECT_EQ(result.observed, "not running");
    EXPECT_EQ(result.message, "expected antivirus.exe to be running");
}

TEST(FailureMessages, AProcessThatShouldBeAbsentSaysSo) {
    const CheckResult result = only_result(evaluate(bundle_with(process_rule(Presence::MustBeAbsent)),
                                               facts_with_process("antivirus.exe"),
                                               all_capabilities()));
    EXPECT_EQ(result.message, "expected antivirus.exe not to be running");
}

TEST(FailureMessages, AVersionMismatchNamesTheConstraint) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("2.0")};
    const CheckResult result =
        only_result(evaluate(bundle_with(process_rule(Presence::MustBePresent, constraint)),
                             facts_with_process("antivirus.exe", parse_version("1.4")),
                             all_capabilities()));

    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_EQ(result.observed, "running, version 1.4");
    EXPECT_EQ(result.message, "expected version >= 2.0");
}

TEST(FailureMessages, RegistryEqualsQuotesTheValueItWanted) {
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.registry[R"(HKLM\SOFTWARE\Acme\\Version)"] = RegistryValue{true, "1.4", ""};

    const CheckResult result = only_result(
        evaluate(bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Equals, "2.0")),
                 facts, all_capabilities()));

    EXPECT_EQ(result.observed, "1.4");
    EXPECT_EQ(result.message, "expected the value to be \"2.0\"");
}

TEST(FailureMessages, RegistryContainsSaysWhatSubstringWasWanted) {
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.registry[R"(HKLM\SOFTWARE\Acme\\Version)"] = RegistryValue{true, "1.4", ""};

    const CheckResult result = only_result(evaluate(
        bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Contains, "beta")), facts,
        all_capabilities()));

    EXPECT_EQ(result.message, "expected the value to contain \"beta\"");
}

TEST(FailureMessages, AnAbsentRegistryValueNamesTheKey) {
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.registry[R"(HKLM\SOFTWARE\Acme\\Version)"] = RegistryValue{false, "", ""};

    const CheckResult result = only_result(
        evaluate(bundle_with(registry_rule(Presence::MustBePresent, RegistryMatch::Exists, "")),
                 facts, all_capabilities()));

    EXPECT_EQ(result.observed, "value absent");
    EXPECT_NE(result.message.find(R"(SOFTWARE\Acme)"), std::string::npos) << result.message;
}

TEST(FailureMessages, PassingResultsCarryNoExpectation) {
    // Repeating the expectation beside a tick is noise, and the compliance
    // views append the message to the observation unconditionally.
    const CheckResult result = only_result(evaluate(bundle_with(process_rule(Presence::MustBePresent)),
                                               facts_with_process("antivirus.exe"),
                                               all_capabilities()));
    EXPECT_EQ(result.status, CheckStatus::Pass);
    EXPECT_TRUE(result.message.empty());
}

TEST(FailureMessages, EveryFailureCarriesOne) {
    // The blanket guarantee: whatever fails, the row can explain itself.
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.registry[R"(HKLM\SOFTWARE\Acme\\Version)"] = RegistryValue{true, "1.4", ""};
    facts.resources.adapters = {adapter("smash-lan", LinkState::NoMedia)};

    TemplateBundle bundle;
    bundle.baseline.rules = {
        process_rule(Presence::MustBePresent),
        service_rule(Presence::MustBePresent),
        registry_rule(Presence::MustBePresent, RegistryMatch::Equals, "2.0"),
        count_rule(Comparison::AtLeast, 1),
        adapter_state_rule("smash-lan", LinkState::Connected),
    };
    // Distinct ids so every rule survives rules_for()'s dedup.
    int n = 0;
    for (Rule& rule : bundle.baseline.rules) {
        rule.id = "rule-" + std::to_string(n++);
    }

    const ComplianceReport report = evaluate(bundle, facts, all_capabilities());
    ASSERT_EQ(report.results.size(), 5u);
    for (const CheckResult& result : report.results) {
        EXPECT_EQ(result.status, CheckStatus::Fail) << result.rule_id;
        EXPECT_FALSE(result.message.empty())
            << result.rule_id << " failed without saying what was expected";
        EXPECT_NE(result.message.find("expected"), std::string::npos) << result.message;
    }
}
