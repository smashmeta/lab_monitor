#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "cart_publisher.hpp"
#include "cart_state.hpp"
#include "lm/core/compliance.hpp"
#include "lm/core/rule.hpp"
#include "lm/transport/dds_probe.hpp"

using namespace lm::core;

namespace {

/// A domain of its own, away from both the fleet's and the other integration
/// test's, so running these while anything else is up reads nobody else's bus.
constexpr std::uint32_t kDomain = 73;
constexpr const char* kTopic = "ShoppingCart";

/// The whole stack a DDS rule actually runs through, end to end: the fixture
/// publishes, the real probe reads with no compile-time knowledge of the type,
/// and the real evaluate() judges it. Nothing here is a stand-in except the
/// application being watched -- which is the one thing that has to be.
CheckResult check(const cart::State& state, Rule rule) {
    cart::Publisher publisher;
    // localhost_only, which is how the fixture actually runs: every case below
    // therefore proves the confined publisher is still readable by an
    // unmodified probe -- the one thing the whitelist could plausibly break.
    const std::string failure = publisher.start(kDomain, kTopic, true);
    EXPECT_TRUE(failure.empty()) << failure;
    EXPECT_TRUE(publisher.publish(state));

    const auto probe = lm::transport::make_dds_probe();
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.dds.emplace(dds_key(kDomain, kTopic), probe->look(kDomain, kTopic));

    TemplateBundle bundle;
    rule.id = "r1";
    bundle.baseline.rules.push_back(std::move(rule));

    Capabilities caps;
    caps.add(Capability::Dds);
    const ComplianceReport report = evaluate(bundle, facts, caps);
    EXPECT_EQ(report.results.size(), 1u);
    return report.results.empty() ? CheckResult{} : report.results.front();
}

Rule value_rule(std::string path, DdsMatch match, std::string expected) {
    Rule rule;
    rule.payload = DdsValueRule{kDomain, kTopic, std::move(path), match, std::move(expected)};
    return rule;
}

cart::State two_items() {
    cart::State state;
    cart::add(state, "A-100", 9.5, 1);
    cart::add(state, "B-200", 12.0, 2);
    return state;
}

}  // namespace

TEST(CartRules, TheMotivatingCasePassesWhenTheCartHoldsTwoLines) {
    // "Basket.items_ contains 2" from the original question, running through
    // every real component between the cart and the verdict.
    const CheckResult result = check(two_items(), value_rule("items_.length", DdsMatch::Equals, "2"));
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.observed << " / " << result.message;
    EXPECT_EQ(result.observed, "2");
}

TEST(CartRules, AndFailsWithTheExpectationSpeltOutWhenItDoesNot) {
    cart::State state;
    cart::add(state, "A-100", 9.5, 1);

    const CheckResult result = check(state, value_rule("items_.length", DdsMatch::Equals, "2"));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_EQ(result.observed, "1");
    EXPECT_EQ(result.message, "expected items_.length equal to 2");
}

TEST(CartRules, CountsUnitsSeparatelyFromLines) {
    // Two lines, three units -- the distinction the cart deliberately keeps,
    // and a rule can ask about either.
    EXPECT_EQ(check(two_items(), value_rule("unit_count", DdsMatch::Equals, "3")).status,
              CheckStatus::Pass);
}

TEST(CartRules, ComparesTheTotalNumerically) {
    // 9.5 + 2 x 12.0 = 33.5, read as a number rather than as text.
    const CheckResult result = check(two_items(), value_rule("total", DdsMatch::AtLeast, "30"));
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.observed;
    EXPECT_EQ(check(two_items(), value_rule("total", DdsMatch::AtMost, "30")).status,
              CheckStatus::Fail);
}

TEST(CartRules, MatchesTheStatusAsText) {
    cart::State state = two_items();
    state.status = "Packing";
    EXPECT_EQ(check(state, value_rule("status", DdsMatch::Equals, "Packing")).status,
              CheckStatus::Pass);
    EXPECT_EQ(check(state, value_rule("status", DdsMatch::Contains, "ack")).status,
              CheckStatus::Pass);
}

TEST(CartRules, ReadsThroughASequenceIntoANestedStructure) {
    EXPECT_EQ(check(two_items(), value_rule("items_[1].sku", DdsMatch::Equals, "B-200")).status,
              CheckStatus::Pass);
}

TEST(CartRules, APathThatAddressesNothingIsAnErrorNamingIt) {
    const CheckResult result = check(two_items(), value_rule("basket_", DdsMatch::Equals, "2"));
    EXPECT_EQ(result.status, CheckStatus::Error);
    EXPECT_NE(result.message.find("basket_"), std::string::npos) << result.message;
}

TEST(CartRules, TheTopicRulePassesWithoutReadingTheDataAtAll) {
    Rule rule;
    rule.payload = DdsTopicRule{kDomain, kTopic};
    EXPECT_EQ(check(cart::State{}, std::move(rule)).status, CheckStatus::Pass);
}
