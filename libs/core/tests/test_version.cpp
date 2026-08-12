#include <gtest/gtest.h>

#include "lm/core/version.hpp"

using namespace lm::core;

TEST(ParseVersion, AcceptsDottedNumbers) {
    const auto v = parse_version("10.0.19045.1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->parts, (std::vector<int>{10, 0, 19045, 1}));
}

TEST(ParseVersion, AcceptsSingleComponent) {
    const auto v = parse_version("7");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->parts, (std::vector<int>{7}));
}

TEST(ParseVersion, RejectsMalformedInput) {
    EXPECT_FALSE(parse_version("").has_value());
    EXPECT_FALSE(parse_version("1..2").has_value());
    EXPECT_FALSE(parse_version("1.2.").has_value());
    EXPECT_FALSE(parse_version("1.x").has_value());
    EXPECT_FALSE(parse_version("-1.2").has_value());
    EXPECT_FALSE(parse_version("v1.2").has_value());
}

TEST(CompareVersion, TrailingZerosAreInsignificant) {
    EXPECT_EQ(compare(*parse_version("1.2"), *parse_version("1.2.0")), 0);
    EXPECT_EQ(compare(*parse_version("1.2.0.0"), *parse_version("1.2")), 0);
}

TEST(CompareVersion, OrdersNumericallyNotLexically) {
    EXPECT_LT(compare(*parse_version("1.9"), *parse_version("1.10")), 0);
    EXPECT_GT(compare(*parse_version("2.0"), *parse_version("1.99")), 0);
}

TEST(Satisfies, HandlesEveryOperator) {
    const Version actual = *parse_version("3.4.1");

    EXPECT_TRUE(satisfies(actual, {ComparisonOp::Equal, *parse_version("3.4.1")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::Equal, *parse_version("3.4.1.0")}));
    EXPECT_FALSE(satisfies(actual, {ComparisonOp::Equal, *parse_version("3.4.2")}));

    EXPECT_TRUE(satisfies(actual, {ComparisonOp::NotEqual, *parse_version("3.4.2")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::GreaterEqual, *parse_version("3.4.1")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::GreaterEqual, *parse_version("3.0")}));
    EXPECT_FALSE(satisfies(actual, {ComparisonOp::Greater, *parse_version("3.4.1")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::Less, *parse_version("4.0")}));
    EXPECT_TRUE(satisfies(actual, {ComparisonOp::LessEqual, *parse_version("3.4.1")}));
}

TEST(ToString, RoundTripsThroughParse) {
    EXPECT_EQ(to_string(*parse_version("10.0.19045")), "10.0.19045");
}
