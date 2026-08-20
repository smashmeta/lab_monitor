#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "lm/core/json_path.hpp"

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
