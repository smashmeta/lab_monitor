#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "lm/core/template_bundle.hpp"

using namespace lm::core;

namespace {

Rule process_rule(std::string exe) {
    Rule rule;
    rule.payload = ProcessRule{std::move(exe)};
    return rule;
}

Rule service_rule(std::string name) {
    Rule rule;
    rule.payload = ServiceRule{std::move(name), std::nullopt};
    return rule;
}

Rule registry_rule(std::string key_path, std::string value_name) {
    RegistryRule payload;
    payload.key_path = std::move(key_path);
    payload.value_name = std::move(value_name);
    Rule rule;
    rule.payload = payload;
    return rule;
}

/// Adds a rule with a fixed id to a named template, creating it if needed.
void add_rule(TemplateBundle& bundle, const std::string& template_name, RuleId id) {
    Rule rule = process_rule("placeholder.exe");
    rule.id = std::move(id);
    if (template_name.empty()) {
        bundle.baseline.rules.push_back(std::move(rule));
        return;
    }
    for (Template& tmpl : bundle.templates) {
        if (tmpl.name == template_name) {
            tmpl.rules.push_back(std::move(rule));
            return;
        }
    }
    Template tmpl;
    tmpl.name = template_name;
    tmpl.rules.push_back(std::move(rule));
    bundle.templates.push_back(std::move(tmpl));
}

}  // namespace

TEST(BaselineName, MatchesTheReservedNameWhateverItsCase) {
    EXPECT_TRUE(is_baseline_name("Baseline"));
    EXPECT_TRUE(is_baseline_name("baseline"));
    EXPECT_TRUE(is_baseline_name("BASELINE"));
}

TEST(BaselineName, DoesNotMatchNamesThatMerelyContainIt) {
    EXPECT_FALSE(is_baseline_name("Baseline 2"));
    EXPECT_FALSE(is_baseline_name("Old Baseline"));
    EXPECT_FALSE(is_baseline_name("Base"));
    EXPECT_FALSE(is_baseline_name(""));
}

TEST(MakeRuleId, DerivesTheIdFromTheProcessName) {
    const TemplateBundle bundle;
    EXPECT_EQ(make_rule_id(bundle, process_rule("chrome.exe")), "process-chrome-exe");
}

TEST(MakeRuleId, DerivesTheIdFromTheServiceName) {
    const TemplateBundle bundle;
    EXPECT_EQ(make_rule_id(bundle, service_rule("Spooler")), "service-spooler");
}

TEST(MakeRuleId, PrefersTheRegistryValueNameOverTheKeyPath) {
    const TemplateBundle bundle;
    EXPECT_EQ(make_rule_id(bundle, registry_rule(R"(SOFTWARE\Acme\Terminal)", "DisplayVersion")),
              "registry-displayversion");
}

TEST(MakeRuleId, FallsBackToTheLastKeySegmentWhenThereIsNoValueName) {
    const TemplateBundle bundle;
    EXPECT_EQ(make_rule_id(bundle, registry_rule(R"(SOFTWARE\Acme\Terminal)", "")),
              "registry-terminal");
}

TEST(MakeRuleId, FallsBackToTheKindAloneWhenTheTargetSlugifiesToNothing) {
    const TemplateBundle bundle;
    EXPECT_EQ(make_rule_id(bundle, process_rule("***")), "process");
}

TEST(MakeRuleId, SuffixesWhenTheSlugIsAlreadyTaken) {
    TemplateBundle bundle;
    add_rule(bundle, "Workstation", "process-chrome-exe");

    EXPECT_EQ(make_rule_id(bundle, process_rule("chrome.exe")), "process-chrome-exe-2");

    add_rule(bundle, "Workstation", "process-chrome-exe-2");
    EXPECT_EQ(make_rule_id(bundle, process_rule("chrome.exe")), "process-chrome-exe-3");
}

TEST(MakeRuleId, TreatsIdsAsTakenAcrossEveryTemplateAndTheBaseline) {
    // rules_for() merges the baseline with every assigned template and keeps
    // only the first rule per id, so uniqueness has to be bundle-wide -- an id
    // free in this template but used in another is not free.
    TemplateBundle bundle;
    add_rule(bundle, "", "process-agent-exe");
    add_rule(bundle, "Build Server", "process-agent-exe-2");

    EXPECT_EQ(make_rule_id(bundle, process_rule("agent.exe")), "process-agent-exe-3");
}

TEST(DeduplicateRuleIds, LeavesABundleWithUniqueIdsAlone) {
    TemplateBundle bundle;
    add_rule(bundle, "", "base-1");
    add_rule(bundle, "Workstation", "ws-1");

    EXPECT_TRUE(deduplicate_rule_ids(bundle).empty());
    EXPECT_EQ(bundle.baseline.rules.front().id, "base-1");
    EXPECT_EQ(bundle.templates.front().rules.front().id, "ws-1");
}

TEST(DeduplicateRuleIds, RenamesTheLaterRuleAndReportsIt) {
    // The first occurrence is the one rules_for() keeps today, so renaming it
    // would move whichever rule the fleet is currently evaluating.
    TemplateBundle bundle;
    add_rule(bundle, "Workstation", "shared");
    add_rule(bundle, "Build Server", "shared");

    const std::vector<std::pair<RuleId, RuleId>> renames = deduplicate_rule_ids(bundle);

    ASSERT_EQ(renames.size(), 1u);
    EXPECT_EQ(renames.front().first, "shared");
    EXPECT_EQ(bundle.templates.at(0).rules.front().id, "shared");
    EXPECT_EQ(bundle.templates.at(1).rules.front().id, renames.front().second);
    EXPECT_NE(bundle.templates.at(1).rules.front().id, "shared");
}

TEST(DeduplicateRuleIds, GivesARuleWithNoIdAtAllAGeneratedOne) {
    TemplateBundle bundle;
    Template tmpl;
    tmpl.name = "Workstation";
    tmpl.rules.push_back(process_rule("labtool.exe"));  // id left empty
    bundle.templates.push_back(std::move(tmpl));

    const std::vector<std::pair<RuleId, RuleId>> renames = deduplicate_rule_ids(bundle);

    ASSERT_EQ(renames.size(), 1u);
    EXPECT_EQ(bundle.templates.front().rules.front().id, "process-labtool-exe");
}

TEST(DeduplicateRuleIds, ProducesIdsThatSurviveASecondPass) {
    TemplateBundle bundle;
    add_rule(bundle, "", "shared");
    add_rule(bundle, "Workstation", "shared");
    add_rule(bundle, "Build Server", "shared");

    EXPECT_EQ(deduplicate_rule_ids(bundle).size(), 2u);
    EXPECT_TRUE(deduplicate_rule_ids(bundle).empty());
}
