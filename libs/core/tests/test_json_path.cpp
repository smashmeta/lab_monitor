#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "lm/core/json_path.hpp"

#include <string>
#include <string_view>
#include <vector>

using namespace lm::core;

namespace {

/// The shape the motivating case has: a basket topic carrying a sequence of
/// items, each with fields of its own.
nlohmann::json basket() {
    return nlohmann::json{{"status", "Ready"},
                          {"owner", nlohmann::json{{"name", "line-3"}, {"shift", 2}}},
                          {"items_", nlohmann::json::array({
                                          nlohmann::json{{"sku", "A-100"}, {"price", 9.5}},
                                          nlohmann::json{{"sku", "B-200"}, {"price", 12.0}},
                                      })}};
}

nlohmann::json value_of(const std::expected<nlohmann::json, PathFailure>& result) {
    EXPECT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    return result.has_value() ? *result : nlohmann::json{};
}

}  // namespace

TEST(JsonPath, ReadsATopLevelField) {
    EXPECT_EQ(value_of(resolve_path(basket(), "status")), "Ready");
}

TEST(JsonPath, ReadsANestedField) {
    EXPECT_EQ(value_of(resolve_path(basket(), "owner.shift")), 2);
}

TEST(JsonPath, ReadsASequenceElement) {
    EXPECT_EQ(value_of(resolve_path(basket(), "items_[1].sku")), "B-200");
}

TEST(JsonPath, LengthCountsSequenceElements) {
    // The motivating case: "the basket contains 2 items".
    EXPECT_EQ(value_of(resolve_path(basket(), "items_.length")), 2);
}

TEST(JsonPath, LengthWorksOnStringsAndObjects) {
    EXPECT_EQ(value_of(resolve_path(basket(), "status.length")), 5);
    EXPECT_EQ(value_of(resolve_path(basket(), "owner.length")), 2);
}

TEST(JsonPath, LengthIsOnlyAProjectionInFinalPosition) {
    // A field genuinely called "length" stays addressable, as long as the path
    // continues past it -- which is what keeps the projection unambiguous.
    const nlohmann::json doc{{"box", nlohmann::json{{"length", nlohmann::json{{"cm", 30}}}}}};
    EXPECT_EQ(value_of(resolve_path(doc, "box.length.cm")), 30);
}

TEST(JsonPath, AnEmptyPathIsTheWholeDocument) {
    EXPECT_EQ(value_of(resolve_path(basket(), "")), basket());
}

TEST(JsonPath, ReportsAMissingFieldAsSuch) {
    const auto result = resolve_path(basket(), "itmes_");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, PathError::NoSuchField);
    EXPECT_NE(result.error().message.find("itmes_"), std::string::npos)
        << "the message must name what was looked for: " << result.error().message;
}

TEST(JsonPath, ReportsAnIndexPastTheEndWithTheActualCount) {
    const auto result = resolve_path(basket(), "items_[4]");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, PathError::NoSuchField);
    EXPECT_NE(result.error().message.find('2'), std::string::npos)
        << "saying how many there are is the whole point: " << result.error().message;
}

TEST(JsonPath, IndexingSomethingThatIsNotASequenceFails) {
    const auto result = resolve_path(basket(), "status[0]");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, PathError::NoSuchField);
}

TEST(JsonPath, DescendingIntoAScalarFails) {
    const auto result = resolve_path(basket(), "status.anything");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, PathError::NoSuchField);
}

TEST(JsonPath, MalformedPathsAreToldApartFromMissingOnes) {
    // An authoring mistake, not a fact about the data.
    for (const char* path : {"items_[", "items_[x]", "items_[0", "a..b", "trailing.", ".leading"}) {
        const auto result = resolve_path(basket(), path);
        ASSERT_FALSE(result.has_value()) << path;
        EXPECT_EQ(result.error().kind, PathError::Malformed) << path;
    }
}

TEST(JsonPath, NeverThrowsOnAnyInput) {
    // It runs against whatever an operator typed, so it has to be total.
    for (const char* path : {"", ".", "[", "]", "[]", "...", "a[0][1]", "length", "["}) {
        EXPECT_NO_THROW({ (void)resolve_path(basket(), path); }) << path;
    }
}

namespace {

/// A cart shaped like the one the shopping_cart fixture publishes.
const nlohmann::json kCart = nlohmann::json::parse(R"({
  "status": "Ready",
  "unit_count": 4,
  "total": 12.5,
  "items_": [
    {"sku": "A-100", "price": 2.5, "quantity": 1},
    {"sku": "bread", "price": 5.0, "quantity": 2},
    {"sku": "milk",  "price": 5.0, "quantity": 1}
  ]
})");

std::vector<std::string> readable_all(const nlohmann::json& document, std::string_view path) {
    const auto values = resolve_all(document, path);
    EXPECT_TRUE(values.has_value()) << (values ? "" : values.error().message);
    std::vector<std::string> text;
    for (const nlohmann::json& value : values.value_or(std::vector<nlohmann::json>{})) {
        text.push_back(value.is_string() ? value.get<std::string>() : value.dump());
    }
    return text;
}

}  // namespace

TEST(JsonPathWildcard, AddressesTheFieldOfEveryElement) {
    EXPECT_EQ(readable_all(kCart, "items_[*].sku"),
              (std::vector<std::string>{"A-100", "bread", "milk"}));
}

TEST(JsonPathWildcard, AddressesWholeElements) {
    const auto values = resolve_all(kCart, "items_[*]");
    ASSERT_TRUE(values.has_value());
    ASSERT_EQ(values->size(), 3u);
    EXPECT_EQ((*values)[1]["sku"], "bread");
}

TEST(JsonPathWildcard, CarriesTheLengthProjectionThrough) {
    // One length per element, not the length of the sequence: the projection
    // applies to whatever the segments before it addressed.
    EXPECT_EQ(readable_all(kCart, "items_[*].sku.length"),
              (std::vector<std::string>{"5", "5", "4"}));
}

TEST(JsonPathWildcard, AnEmptySequenceAddressesNothingAndThatIsNotAnError) {
    // The distinction the whole feature turns on: "no element has that sku"
    // is an answer, and a rule must be able to Fail on it rather than Error.
    const nlohmann::json empty = nlohmann::json::parse(R"({"items_": []})");
    const auto values = resolve_all(empty, "items_[*].sku");
    ASSERT_TRUE(values.has_value()) << values.error().message;
    EXPECT_TRUE(values->empty());
}

TEST(JsonPathWildcard, SkipsElementsMissingTheFieldWhenOthersHaveIt) {
    // A sequence whose members differ is data, not an authoring mistake.
    const nlohmann::json mixed =
        nlohmann::json::parse(R"({"items_": [{"sku": "A-100"}, {"note": "no sku here"}]})");
    EXPECT_EQ(readable_all(mixed, "items_[*].sku"), (std::vector<std::string>{"A-100"}));
}

TEST(JsonPathWildcard, ReportsTheMissingFieldWhenNoElementHasIt) {
    // The typo case -- items_[*].skew -- which has to say what it could not
    // find rather than quietly addressing nothing.
    const auto values = resolve_all(kCart, "items_[*].skew");
    ASSERT_FALSE(values.has_value());
    EXPECT_EQ(values.error().kind, PathError::NoSuchField);
    EXPECT_NE(values.error().message.find("skew"), std::string::npos) << values.error().message;
}

TEST(JsonPathWildcard, RefusesToIndexSomethingThatIsNotASequence) {
    const auto values = resolve_all(kCart, "status[*]");
    ASSERT_FALSE(values.has_value());
    EXPECT_NE(values.error().message.find("not a sequence"), std::string::npos)
        << values.error().message;
}

TEST(JsonPathWildcard, StillRejectsAnIndexThatIsNeitherANumberNorAStar) {
    const auto values = resolve_all(kCart, "items_[x].sku");
    ASSERT_FALSE(values.has_value());
    EXPECT_EQ(values.error().kind, PathError::Malformed);
    EXPECT_NE(values.error().message.find("*"), std::string::npos) << values.error().message;
}

TEST(JsonPathWildcard, ResolvePathStillReturnsTheOneValueForASingularPath) {
    const auto value = resolve_path(kCart, "items_[1].sku");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "bread");
}

TEST(JsonPathWildcard, ResolvePathRefusesAPathThatAddressesSeveral) {
    // Rather than silently picking the first, which would make items_[*].sku
    // look like it had worked.
    const auto value = resolve_path(kCart, "items_[*].sku");
    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().kind, PathError::Malformed);
}
