#include <gtest/gtest.h>

#include "lm/core/json.hpp"

using namespace lm::core;

namespace {

TemplateBundle sample_bundle() {
    TemplateBundle bundle;
    bundle.revision = 7;

    Rule process;
    process.id = "r1";
    process.description = "Antivirus must run";
    process.expectation = Presence::MustBePresent;
    process.payload = ProcessRule{"antivirus.exe"};
    process.version = VersionConstraint{ComparisonOp::GreaterEqual, *parse_version("2.1")};

    Rule service;
    service.id = "r2";
    service.description = "Telnet must be absent";
    service.expectation = Presence::MustBeAbsent;
    service.payload = ServiceRule{"telnet", std::nullopt};

    Rule registry;
    registry.id = "r3";
    registry.description = "Tool version pinned";
    registry.expectation = Presence::MustBePresent;
    registry.payload = RegistryRule{RegistryHive::LocalMachine, "SOFTWARE\\Acme",
                                    "Version", RegistryMatch::Equals, "4.2"};

    bundle.baseline.name = "baseline";
    bundle.baseline.rules = {process};

    Template workstation;
    workstation.name = "Lab Workstation";
    workstation.rules = {service, registry};
    bundle.templates = {workstation};
    bundle.assignments["PC-001"] = {"Lab Workstation"};
    return bundle;
}

}  // namespace

TEST(BundleJson, RoundTripsExactly) {
    const TemplateBundle original = sample_bundle();
    const std::string text = serialise_bundle(original);

    const auto parsed = parse_bundle(text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, original);
}

TEST(BundleJson, RoundTripsAnEmptyBundle) {
    const TemplateBundle empty;
    const auto parsed = parse_bundle(serialise_bundle(empty));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, empty);
}

TEST(BundleJson, ReportsMalformedJsonAsError) {
    const auto parsed = parse_bundle("{ not json");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_FALSE(parsed.error().empty());
}

TEST(BundleJson, ReportsUnknownEnumAsError) {
    const auto parsed = parse_bundle(R"({"revision":1,"hash":"","baseline":{"name":"b",
        "rules":[{"id":"x","description":"","expectation":"Sometimes",
        "payload":{"type":"process","executable":"a.exe"}}]},
        "templates":[],"assignments":{}})");
    ASSERT_FALSE(parsed.has_value());
}

TEST(ContentHash, IsStableAcrossSerialisation) {
    const TemplateBundle original = sample_bundle();
    const auto parsed = parse_bundle(serialise_bundle(original));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(content_hash(original), content_hash(*parsed));
}

TEST(ContentHash, IgnoresRevisionAndHashFields) {
    TemplateBundle a = sample_bundle();
    TemplateBundle b = sample_bundle();
    b.revision = 999;
    b.hash = "stale";
    EXPECT_EQ(content_hash(a), content_hash(b));
}

TEST(ContentHash, ChangesWhenARuleChanges) {
    const TemplateBundle a = sample_bundle();
    TemplateBundle b = sample_bundle();
    b.templates[0].rules[0].expectation = Presence::MustBePresent;
    EXPECT_NE(content_hash(a), content_hash(b));
}

TEST(ContentHash, ChangesWhenAnAssignmentChanges) {
    const TemplateBundle a = sample_bundle();
    TemplateBundle b = sample_bundle();
    b.assignments["PC-002"] = {"Lab Workstation"};
    EXPECT_NE(content_hash(a), content_hash(b));
}

// --- network rules ----------------------------------------------------------

TEST(Json, AdapterCountRuleRoundTrips) {
    TemplateBundle bundle;
    bundle.revision = 7;
    Rule rule;
    rule.id = "network-adapters";
    rule.description = "At least one network connection";
    rule.payload = AdapterCountRule{Comparison::AtLeast, 1};
    bundle.baseline.rules.push_back(rule);

    const auto parsed = parse_bundle(serialise_bundle(bundle));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, bundle);
    EXPECT_EQ(kind_of(parsed->baseline.rules.front()), RuleKind::Network);
}

TEST(Json, AdapterStateRuleRoundTrips) {
    TemplateBundle bundle;
    Rule rule;
    rule.id = "network-smash-wifi";
    rule.expectation = Presence::MustBeAbsent;
    rule.payload = AdapterStateRule{"smash-wifi", LinkState::NoMedia};
    bundle.baseline.rules.push_back(rule);

    const auto parsed = parse_bundle(serialise_bundle(bundle));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, bundle);
}

TEST(Json, EveryComparisonAndLinkStateSurvivesTheRoundTrip) {
    // The wire names are deliberately not the display strings, so every value
    // needs to be exercised rather than assumed.
    for (const Comparison comparison : {Comparison::AtLeast, Comparison::Exactly, Comparison::AtMost}) {
        TemplateBundle bundle;
        Rule rule;
        rule.id = "n";
        rule.payload = AdapterCountRule{comparison, 3};
        bundle.baseline.rules.push_back(rule);
        const auto parsed = parse_bundle(serialise_bundle(bundle));
        ASSERT_TRUE(parsed.has_value()) << parsed.error();
        EXPECT_EQ(*parsed, bundle);
    }

    for (const LinkState state : {LinkState::Unknown, LinkState::Connected, LinkState::NoMedia,
                                  LinkState::Disconnected, LinkState::Connecting,
                                  LinkState::Disabled, LinkState::Faulted}) {
        TemplateBundle bundle;
        Rule rule;
        rule.id = "n";
        rule.payload = AdapterStateRule{"nic", state};
        bundle.baseline.rules.push_back(rule);
        const auto parsed = parse_bundle(serialise_bundle(bundle));
        ASSERT_TRUE(parsed.has_value()) << parsed.error();
        EXPECT_EQ(*parsed, bundle);
    }
}

TEST(Json, RejectsAnUnknownLinkStateRatherThanGuessing) {
    const std::string text = R"({"revision":1,"hash":"","baseline":{"name":"baseline","rules":[
        {"id":"n","description":"","expectation":"MustBePresent",
         "payload":{"type":"adapter_state","adapter_name":"nic","expected":"Teleported"}}]},
        "templates":[],"assignments":{}})";

    EXPECT_FALSE(parse_bundle(text).has_value());
}
