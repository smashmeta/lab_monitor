#include <gtest/gtest.h>

#include <algorithm>

#include "lm/core/template_bundle.hpp"

using namespace lm::core;

namespace {

Rule process_rule(RuleId id, std::string exe, Presence presence = Presence::MustBePresent) {
    Rule rule;
    rule.id = std::move(id);
    rule.description = "process " + exe;
    rule.expectation = presence;
    rule.payload = ProcessRule{std::move(exe)};
    return rule;
}

std::vector<RuleId> ids_of(const std::vector<const Rule*>& rules) {
    std::vector<RuleId> out;
    out.reserve(rules.size());
    for (const Rule* rule : rules) {
        out.push_back(rule->id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

TemplateBundle make_bundle() {
    TemplateBundle bundle;
    bundle.revision = 4;
    bundle.baseline.name = "baseline";
    bundle.baseline.rules.push_back(process_rule("base-1", "antivirus.exe"));

    Template workstation;
    workstation.name = "Lab Workstation";
    workstation.rules.push_back(process_rule("ws-1", "labtool.exe"));
    workstation.rules.push_back(process_rule("shared-1", "agent.exe"));

    Template build_server;
    build_server.name = "Build Server";
    build_server.rules.push_back(process_rule("bs-1", "buildd.exe"));
    build_server.rules.push_back(process_rule("shared-1", "agent.exe"));

    bundle.templates = {workstation, build_server};
    bundle.assignments["PC-001"] = {"Lab Workstation"};
    bundle.assignments["PC-002"] = {"Lab Workstation", "Build Server"};
    bundle.assignments["PC-003"] = {"Nonexistent Template"};
    return bundle;
}

}  // namespace

TEST(KindOf, DerivesFromPayload) {
    EXPECT_EQ(kind_of(process_rule("a", "x.exe")), RuleKind::Process);

    Rule service;
    service.payload = ServiceRule{"spooler", ServiceState::Running};
    EXPECT_EQ(kind_of(service), RuleKind::Service);

    Rule registry;
    registry.payload = RegistryRule{RegistryHive::LocalMachine, "SOFTWARE\\Acme",
                                    "Version", RegistryMatch::Exists, ""};
    EXPECT_EQ(kind_of(registry), RuleKind::Registry);
}

TEST(RulesFor, UnassignedHostGetsBaselineOnly) {
    const TemplateBundle bundle = make_bundle();
    EXPECT_EQ(ids_of(rules_for(bundle, "UNKNOWN-PC")), (std::vector<RuleId>{"base-1"}));
}

TEST(RulesFor, AppliesBaselinePlusAssignedTemplate) {
    const TemplateBundle bundle = make_bundle();
    EXPECT_EQ(ids_of(rules_for(bundle, "PC-001")),
              (std::vector<RuleId>{"base-1", "shared-1", "ws-1"}));
}

TEST(RulesFor, DeduplicatesRulesSharedByTwoTemplates) {
    const TemplateBundle bundle = make_bundle();
    EXPECT_EQ(ids_of(rules_for(bundle, "PC-002")),
              (std::vector<RuleId>{"base-1", "bs-1", "shared-1", "ws-1"}));
}

TEST(RulesFor, IgnoresAssignmentsToMissingTemplates) {
    const TemplateBundle bundle = make_bundle();
    EXPECT_EQ(ids_of(rules_for(bundle, "PC-003")), (std::vector<RuleId>{"base-1"}));
}

TEST(RulesFor, EmptyBundleYieldsNoRules) {
    const TemplateBundle empty;
    EXPECT_TRUE(rules_for(empty, "PC-001").empty());
}

TEST(RegistryKey, BuildsCanonicalLookupPath) {
    const RegistryRule rule{RegistryHive::LocalMachine, "SOFTWARE\\Acme\\Tool",
                            "Version", RegistryMatch::Exists, ""};
    EXPECT_EQ(registry_key(rule), "HKLM\\SOFTWARE\\Acme\\Tool\\\\Version");
}
