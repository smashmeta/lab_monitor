#include <gtest/gtest.h>

#include "rule_detail.hpp"

using namespace lm::core;

namespace {

Rule process_rule(std::string description, std::optional<VersionConstraint> version = std::nullopt) {
    Rule rule;
    rule.id = "p1";
    rule.description = std::move(description);
    rule.expectation = Presence::MustBePresent;
    rule.payload = ProcessRule{"antivirus.exe"};
    rule.version = std::move(version);
    return rule;
}

}  // namespace

TEST(DescribeRule, UsesTheAuthoredDescriptionAsTheLabel) {
    const RuleDetail detail = describe(process_rule("Antivirus must be running"));

    EXPECT_EQ(detail.label, QStringLiteral("Antivirus must be running"));
    EXPECT_EQ(detail.id, QStringLiteral("p1"));
}

TEST(DescribeRule, FallsBackToAGeneratedSummaryWhenDescriptionIsBlank) {
    // The server's Add Rule dialog lets the description be left empty. Showing
    // an empty label would be worse than the rule id it replaces, so fall back
    // to something that identifies the rule at a glance.
    const RuleDetail detail = describe(process_rule(""));

    EXPECT_FALSE(detail.label.isEmpty());
    EXPECT_NE(detail.label.indexOf(QStringLiteral("antivirus.exe")), -1) << detail.label.toStdString();
}

TEST(DescribeRule, ReportsProcessRulesAsApplications) {
    const RuleDetail detail = describe(process_rule("x"));

    EXPECT_EQ(detail.kind, QStringLiteral("Application"));
    EXPECT_EQ(detail.target, QStringLiteral("antivirus.exe"));
    EXPECT_EQ(detail.expectation, QStringLiteral("Must be present"));
    EXPECT_TRUE(detail.constraint.isEmpty());
}

TEST(DescribeRule, RendersAVersionConstraintReadably) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("2.1")};
    const RuleDetail detail = describe(process_rule("x", constraint));

    EXPECT_EQ(detail.constraint, QStringLiteral("version >= 2.1"));
}

TEST(DescribeRule, RendersMustBeAbsent) {
    Rule rule = process_rule("x");
    rule.expectation = Presence::MustBeAbsent;

    EXPECT_EQ(describe(rule).expectation, QStringLiteral("Must be absent"));
}

TEST(DescribeRule, ReportsServiceRulesWithTheirExpectedState) {
    Rule rule;
    rule.id = "s1";
    rule.description = "Spooler running";
    rule.expectation = Presence::MustBePresent;
    rule.payload = ServiceRule{"Spooler", ServiceState::Running};

    const RuleDetail detail = describe(rule);

    EXPECT_EQ(detail.kind, QStringLiteral("Service"));
    EXPECT_EQ(detail.target, QStringLiteral("Spooler"));
    EXPECT_EQ(detail.constraint, QStringLiteral("state Running"));
}

TEST(DescribeRule, RendersRegistryTargetsInReadableHiveForm) {
    Rule rule;
    rule.id = "g1";
    rule.description = "Tool version pinned";
    rule.expectation = Presence::MustBePresent;
    rule.payload = RegistryRule{RegistryHive::LocalMachine, "SOFTWARE\\Acme", "Version",
                                RegistryMatch::Equals, "4.2"};

    const RuleDetail detail = describe(rule);

    EXPECT_EQ(detail.kind, QStringLiteral("Registry"));
    // Single backslashes for display: registry_key()'s doubled separator is an
    // internal lookup key, not something to show an operator.
    EXPECT_EQ(detail.target, QStringLiteral("HKLM\\SOFTWARE\\Acme\\Version"));
    EXPECT_EQ(detail.constraint, QStringLiteral("equals \"4.2\""));
}

TEST(DescribeRule, NamesTheDefaultRegistryValueExplicitly) {
    Rule rule;
    rule.id = "g2";
    rule.expectation = Presence::MustBePresent;
    rule.payload = RegistryRule{RegistryHive::CurrentUser, "SOFTWARE\\Acme", "",
                                RegistryMatch::Exists, ""};

    const RuleDetail detail = describe(rule);

    // An empty value name addresses the key's unnamed default value; showing
    // nothing there would read as a truncated path.
    EXPECT_EQ(detail.target, QStringLiteral("HKCU\\SOFTWARE\\Acme\\(Default)"));
    EXPECT_EQ(detail.constraint, QStringLiteral("exists"));
}

TEST(DescribeRule, RendersRegistryContainsMatch) {
    Rule rule;
    rule.id = "g3";
    rule.expectation = Presence::MustBePresent;
    rule.payload = RegistryRule{RegistryHive::LocalMachine, "SOFTWARE\\Acme", "Channel",
                                RegistryMatch::Contains, "beta"};

    EXPECT_EQ(describe(rule).constraint, QStringLiteral("contains \"beta\""));
}

TEST(DescribeRule, TooltipCarriesEveryFieldAnOperatorNeeds) {
    const VersionConstraint constraint{ComparisonOp::GreaterEqual, *parse_version("2.1")};
    Rule rule = process_rule("Antivirus must be running", constraint);

    const QString tooltip = describe(rule).tooltip();

    EXPECT_NE(tooltip.indexOf(QStringLiteral("Antivirus must be running")), -1) << tooltip.toStdString();
    EXPECT_NE(tooltip.indexOf(QStringLiteral("Application")), -1) << tooltip.toStdString();
    EXPECT_NE(tooltip.indexOf(QStringLiteral("antivirus.exe")), -1) << tooltip.toStdString();
    EXPECT_NE(tooltip.indexOf(QStringLiteral("Must be present")), -1) << tooltip.toStdString();
    EXPECT_NE(tooltip.indexOf(QStringLiteral("version >= 2.1")), -1) << tooltip.toStdString();
    // The rule id stays available for support conversations, just not as the label.
    EXPECT_NE(tooltip.indexOf(QStringLiteral("p1")), -1) << tooltip.toStdString();
}
