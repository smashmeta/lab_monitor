#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lm/core/compliance.hpp"
#include "lm/core/json.hpp"
#include "lm/core/rule.hpp"

using namespace lm::core;

namespace {

constexpr std::uint32_t kDomain = 42;

Capabilities all_capabilities() {
    Capabilities caps;
    caps.add(Capability::Resources)
        .add(Capability::Processes)
        .add(Capability::Services)
        .add(Capability::Registry)
        .add(Capability::Network)
        .add(Capability::Dds);
    return caps;
}

TemplateBundle bundle_with(Rule rule) {
    TemplateBundle bundle;
    bundle.baseline.rules.push_back(std::move(rule));
    return bundle;
}

Rule topic_rule(Presence expectation = Presence::MustBePresent) {
    Rule rule;
    rule.id = "r1";
    rule.description = "A basket topic is on the line bus";
    rule.expectation = expectation;
    rule.payload = DdsTopicRule{kDomain, "Basket"};
    return rule;
}

Rule value_rule(std::string path, DdsMatch match, std::string expected) {
    Rule rule;
    rule.id = "r1";
    rule.description = "The basket holds what it should";
    rule.payload = DdsValueRule{kDomain, "Basket", std::move(path), match, std::move(expected)};
    return rule;
}

/// A host whose basket topic is on the bus and carrying this sample.
HostFacts facts_with_sample(const std::string& json) {
    HostFacts facts;
    facts.host_id = "PC-001";
    DdsTopicSample sample;
    sample.topic_found = true;
    sample.has_sample = true;
    sample.json = json;
    facts.dds.emplace(dds_key(kDomain, "Basket"), std::move(sample));
    return facts;
}

HostFacts facts_with(DdsTopicSample sample) {
    HostFacts facts;
    facts.host_id = "PC-001";
    facts.dds.emplace(dds_key(kDomain, "Basket"), std::move(sample));
    return facts;
}

const char* kBasket = R"({"status":"Ready","items_":[{"sku":"A-100"},{"sku":"B-200"}]})";

/// Returns by value: the report the call produced is a temporary, and a
/// reference into it dangles the moment the statement ends.
CheckResult only_result(const TemplateBundle& bundle, const HostFacts& facts) {
    const ComplianceReport report = evaluate(bundle, facts, all_capabilities());
    EXPECT_EQ(report.results.size(), 1u);
    return report.results.empty() ? CheckResult{} : report.results.front();
}

}  // namespace

TEST(EvaluateDdsTopic, PassesWhenTheTopicIsOnTheBus) {
    DdsTopicSample sample;
    sample.topic_found = true;
    const CheckResult result = only_result(bundle_with(topic_rule()), facts_with(sample));
    EXPECT_EQ(result.status, CheckStatus::Pass);
    EXPECT_EQ(result.observed, "present on the bus");
}

TEST(EvaluateDdsTopic, FailsWhenItIsNotAndSaysWhatWasExpected) {
    const CheckResult result = only_result(bundle_with(topic_rule()), facts_with(DdsTopicSample{}));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_EQ(result.observed, "not on the bus");
    EXPECT_EQ(result.message, "expected \"Basket\" on domain 42 to exist");
}

TEST(EvaluateDdsTopic, MustBeAbsentReadsTheOtherWayRound) {
    DdsTopicSample sample;
    sample.topic_found = true;
    const CheckResult result =
        only_result(bundle_with(topic_rule(Presence::MustBeAbsent)), facts_with(sample));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_EQ(result.message, "expected \"Basket\" on domain 42 not to exist");
}

TEST(EvaluateDdsTopic, PresenceNeedsNoSampleAtAll) {
    // Discovery answers this one, so it works against a publisher that
    // describes nothing about its data and has never published.
    DdsTopicSample sample;
    sample.topic_found = true;
    sample.has_sample = false;
    EXPECT_EQ(only_result(bundle_with(topic_rule()), facts_with(sample)).status, CheckStatus::Pass);
}

TEST(EvaluateDdsValue, CountsSequenceElements) {
    // The motivating case: Basket.items_ must hold exactly 2.
    const CheckResult result = only_result(
        bundle_with(value_rule("items_.length", DdsMatch::Equals, "2")), facts_with_sample(kBasket));
    EXPECT_EQ(result.status, CheckStatus::Pass);
    EXPECT_EQ(result.observed, "2");
}

TEST(EvaluateDdsValue, FailingCountSaysWhatWasExpected) {
    const CheckResult result = only_result(
        bundle_with(value_rule("items_.length", DdsMatch::Equals, "3")), facts_with_sample(kBasket));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_EQ(result.observed, "2");
    EXPECT_EQ(result.message, "expected items_.length equal to 3");
}

TEST(EvaluateDdsValue, ComparesNumbersWithAtLeastAndAtMost) {
    EXPECT_EQ(only_result(bundle_with(value_rule("items_.length", DdsMatch::AtLeast, "2")),
                          facts_with_sample(kBasket))
                  .status,
              CheckStatus::Pass);
    EXPECT_EQ(only_result(bundle_with(value_rule("items_.length", DdsMatch::AtMost, "1")),
                          facts_with_sample(kBasket))
                  .status,
              CheckStatus::Fail);
}

TEST(EvaluateDdsValue, ComparesStrings) {
    EXPECT_EQ(only_result(bundle_with(value_rule("status", DdsMatch::Equals, "Ready")),
                          facts_with_sample(kBasket))
                  .status,
              CheckStatus::Pass);
    EXPECT_EQ(only_result(bundle_with(value_rule("status", DdsMatch::Contains, "ead")),
                          facts_with_sample(kBasket))
                  .status,
              CheckStatus::Pass);
}

TEST(EvaluateDdsValue, ReadsThroughSequenceElements) {
    const CheckResult result = only_result(
        bundle_with(value_rule("items_[1].sku", DdsMatch::Equals, "B-200")), facts_with_sample(kBasket));
    EXPECT_EQ(result.status, CheckStatus::Pass);
}

TEST(EvaluateDdsValue, ANumericMatchAgainstTextIsAnErrorThatSaysSo) {
    // Not a quiet Fail: the rule cannot be answered as written, and the author
    // needs to know which half is wrong.
    const CheckResult result = only_result(bundle_with(value_rule("status", DdsMatch::AtLeast, "2")),
                                            facts_with_sample(kBasket));
    EXPECT_EQ(result.status, CheckStatus::Error);
    EXPECT_NE(result.message.find("found text"), std::string::npos) << result.message;
}

TEST(EvaluateDdsValue, AMissingFieldIsAnErrorNamingTheField) {
    const CheckResult result = only_result(
        bundle_with(value_rule("itmes_.length", DdsMatch::Equals, "2")), facts_with_sample(kBasket));
    EXPECT_EQ(result.status, CheckStatus::Error);
    EXPECT_NE(result.message.find("itmes_"), std::string::npos) << result.message;
}

TEST(EvaluateDdsValue, AMissingTopicFailsRatherThanErrors) {
    // Data that is meant to be on the bus and is not is exactly what a fleet
    // check should catch -- the same call made for a renamed adapter.
    const CheckResult result = only_result(
        bundle_with(value_rule("items_.length", DdsMatch::Equals, "2")), facts_with(DdsTopicSample{}));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.message.find("to be publishing"), std::string::npos) << result.message;
}

TEST(EvaluateDdsValue, TopicPresentButSilentIsAnError) {
    // Nothing is wrong with the machine and nothing can be said about the
    // value, which is the definition of Error here.
    DdsTopicSample sample;
    sample.topic_found = true;
    sample.has_sample = false;
    const CheckResult result =
        only_result(bundle_with(value_rule("items_.length", DdsMatch::Equals, "2")), facts_with(sample));
    EXPECT_EQ(result.status, CheckStatus::Error);
    EXPECT_NE(result.message.find("nothing has been published"), std::string::npos) << result.message;
}

TEST(EvaluateDdsValue, AProbeFailureIsReportedVerbatim) {
    DdsTopicSample sample;
    sample.error = "the publisher advertises no type description";
    const CheckResult result =
        only_result(bundle_with(value_rule("items_.length", DdsMatch::Equals, "2")), facts_with(sample));
    EXPECT_EQ(result.status, CheckStatus::Error);
    EXPECT_EQ(result.message, "the publisher advertises no type description");
}

TEST(EvaluateDdsValue, AnUnprobedTopicIsAnErrorNotAFailure) {
    HostFacts facts;
    facts.host_id = "PC-001";
    const CheckResult result =
        only_result(bundle_with(value_rule("items_.length", DdsMatch::Equals, "2")), facts);
    EXPECT_EQ(result.status, CheckStatus::Error);
}

TEST(EvaluateDds, WithoutTheCapabilityBothKindsAreNotApplicable) {
    Capabilities none;
    for (Rule rule : {topic_rule(), value_rule("items_.length", DdsMatch::Equals, "2")}) {
        const ComplianceReport report =
            evaluate(bundle_with(std::move(rule)), facts_with_sample(kBasket), none);
        ASSERT_EQ(report.results.size(), 1u);
        EXPECT_EQ(report.results.front().status, CheckStatus::NotApplicable);
        EXPECT_NE(report.results.front().observed.find("DDS"), std::string::npos)
            << report.results.front().observed;
    }
}

TEST(EvaluateDds, BothPayloadsMapToTheDdsKindAndCapability) {
    EXPECT_EQ(kind_of(topic_rule()), RuleKind::Dds);
    EXPECT_EQ(kind_of(value_rule("x", DdsMatch::Equals, "1")), RuleKind::Dds);
    EXPECT_EQ(required_capability(RuleKind::Dds), Capability::Dds);
}

TEST(DdsRuleJson, SurvivesARoundTripThroughABundle) {
    TemplateBundle bundle;
    bundle.baseline.rules.push_back(topic_rule());
    bundle.baseline.rules.push_back(value_rule("items_.length", DdsMatch::AtLeast, "2"));
    bundle.baseline.rules.back().id = "r2";

    const auto parsed = parse_bundle(serialise_bundle(bundle));
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(*parsed, bundle);
}

TEST(DdsRuleId, IsGeneratedFromTheTopicAndPath) {
    TemplateBundle bundle;
    EXPECT_EQ(make_rule_id(bundle, topic_rule()), "dds-basket");
    EXPECT_EQ(make_rule_id(bundle, value_rule("items_.length", DdsMatch::Equals, "2")),
              "dds-basket-items-length");
}

namespace {

/// The motivating shape: a basket whose lines each carry a sku and a price.
const char* kStockedBasket =
    R"({"status":"Ready","items_":[{"sku":"A-100","price":2.5},)"
    R"({"sku":"bread","price":5.0},{"sku":"milk","price":1.25}]})";

}  // namespace

TEST(EvaluateDdsAny, PassesWhenOneElementCarriesTheValue) {
    // "at least one line in the basket is bread", which is the question the
    // wildcard was added for.
    const CheckResult result =
        only_result(bundle_with(value_rule("items_[*].sku", DdsMatch::Equals, "bread")),
                    facts_with_sample(kStockedBasket));

    EXPECT_EQ(result.status, CheckStatus::Pass);
}

TEST(EvaluateDdsAny, FailsWhenNoElementCarriesTheValue) {
    const CheckResult result =
        only_result(bundle_with(value_rule("items_[*].sku", DdsMatch::Equals, "caviar")),
                    facts_with_sample(kStockedBasket));

    EXPECT_EQ(result.status, CheckStatus::Fail);
    // Says "any", so the reader is not left thinking one particular line was
    // meant to be caviar.
    EXPECT_NE(result.message.find("any items_[*].sku"), std::string::npos) << result.message;
}

TEST(EvaluateDdsAny, ListsWhatItActuallySawWhenItFails) {
    const CheckResult result =
        only_result(bundle_with(value_rule("items_[*].sku", DdsMatch::Equals, "caviar")),
                    facts_with_sample(kStockedBasket));

    EXPECT_NE(result.observed.find("3 values"), std::string::npos) << result.observed;
    EXPECT_NE(result.observed.find("bread"), std::string::npos) << result.observed;
}

TEST(EvaluateDdsAny, AnEmptySequenceFailsRatherThanErroring) {
    // Nothing is wrong with the machine: there is simply no line that matches,
    // because there are no lines. That is a finding, and the fleet should read
    // it as one.
    const CheckResult result =
        only_result(bundle_with(value_rule("items_[*].sku", DdsMatch::Equals, "bread")),
                    facts_with_sample(R"({"status":"Ready","items_":[]})"));

    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_EQ(result.observed, "no elements");
}

TEST(EvaluateDdsAny, ComparesNumbersAcrossElementsToo) {
    // Not only text: "some line costs at least 5".
    EXPECT_EQ(only_result(bundle_with(value_rule("items_[*].price", DdsMatch::AtLeast, "5")),
                          facts_with_sample(kStockedBasket))
                  .status,
              CheckStatus::Pass);
    EXPECT_EQ(only_result(bundle_with(value_rule("items_[*].price", DdsMatch::AtLeast, "99")),
                          facts_with_sample(kStockedBasket))
                  .status,
              CheckStatus::Fail);
}

TEST(EvaluateDdsAny, MatchesOnASubstringOfAnyElement) {
    EXPECT_EQ(only_result(bundle_with(value_rule("items_[*].sku", DdsMatch::Contains, "rea")),
                          facts_with_sample(kStockedBasket))
                  .status,
              CheckStatus::Pass);
}

TEST(EvaluateDdsAny, AMisspeltFieldIsAnErrorNamingIt) {
    // The authoring mistake the wildcard makes easy: every element lacks the
    // field, so nothing was addressed and the rule cannot be answered.
    const CheckResult result =
        only_result(bundle_with(value_rule("items_[*].skew", DdsMatch::Equals, "bread")),
                    facts_with_sample(kStockedBasket));

    EXPECT_EQ(result.status, CheckStatus::Error);
    EXPECT_NE(result.message.find("skew"), std::string::npos) << result.message;
}

TEST(EvaluateDdsAny, ANumericRuleAgainstTextIsStillAnError) {
    const CheckResult result =
        only_result(bundle_with(value_rule("items_[*].sku", DdsMatch::AtLeast, "5")),
                    facts_with_sample(kStockedBasket));

    EXPECT_EQ(result.status, CheckStatus::Error);
}

TEST(EvaluateDdsAny, OneOddMemberDoesNotTurnARealAnswerIntoAnError) {
    // A sequence with a stray non-numeric member still says whether any member
    // matched -- so the answer stands rather than collapsing into "could not
    // check". Only a sequence where *nothing* could be compared is an Error.
    const char* mixed =
        R"({"items_":[{"price":"free"},{"price":7.0}]})";
    EXPECT_EQ(only_result(bundle_with(value_rule("items_[*].price", DdsMatch::AtLeast, "5")),
                          facts_with_sample(mixed))
                  .status,
              CheckStatus::Pass);
}

TEST(EvaluateDdsAny, LeavesSingularPathsExactlyAsTheyWere) {
    // The regression that matters: adding a plural form must not change what a
    // singular path means, and its observed value is still the bare reading.
    const CheckResult result =
        only_result(bundle_with(value_rule("items_[1].sku", DdsMatch::Equals, "bread")),
                    facts_with_sample(kStockedBasket));

    EXPECT_EQ(result.status, CheckStatus::Pass);
    EXPECT_EQ(result.observed, "bread");
}
