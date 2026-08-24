#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "cart_state.hpp"

namespace {

std::string value_at(const cart::State& state, const std::string& path) {
    const auto paths = cart::rule_paths(state);
    const auto found = std::ranges::find(paths, path, &std::pair<std::string, std::string>::first);
    return found == paths.end() ? std::string{"<no such path>"} : found->second;
}

}  // namespace

TEST(CartState, AddsAnItem) {
    cart::State state;
    cart::add(state, "A-100", 9.5, 1);

    ASSERT_EQ(state.items.size(), 1u);
    EXPECT_EQ(state.items.front().sku, "A-100");
    EXPECT_DOUBLE_EQ(state.items.front().price, 9.5);
}

TEST(CartState, MergesTheSameSkuIntoOneLine) {
    // Otherwise items_.length counts clicks rather than distinct products, and
    // a rule written against it means something different every time.
    cart::State state;
    cart::add(state, "A-100", 9.5, 1);
    cart::add(state, "A-100", 9.5, 2);

    ASSERT_EQ(state.items.size(), 1u);
    EXPECT_EQ(state.items.front().quantity, 3);
}

TEST(CartState, KeepsTheFirstPriceWhenALineIsMergedInto) {
    // The cart is not a price list. Whatever the line was added at is what the
    // total is computed from, so re-adding cannot silently re-price it.
    cart::State state;
    cart::add(state, "A-100", 9.5, 1);
    cart::add(state, "A-100", 99.0, 1);

    EXPECT_DOUBLE_EQ(state.items.front().price, 9.5);
    EXPECT_DOUBLE_EQ(cart::total(state), 19.0);
}

TEST(CartState, RemovesByExactSku) {
    cart::State state;
    cart::add(state, "A-100", 1.0, 1);
    cart::add(state, "B-200", 2.0, 1);

    EXPECT_TRUE(cart::remove(state, "A-100"));
    EXPECT_FALSE(cart::remove(state, "a-100")) << "SKUs are codes, not names";
    ASSERT_EQ(state.items.size(), 1u);
    EXPECT_EQ(state.items.front().sku, "B-200");
}

TEST(CartState, CountsUnitsAndLinesSeparately) {
    cart::State state;
    cart::add(state, "A-100", 1.0, 3);
    cart::add(state, "B-200", 1.0, 1);

    EXPECT_EQ(state.items.size(), 2u) << "two lines";
    EXPECT_EQ(cart::unit_count(state), 4) << "four units";
}

TEST(CartState, TotalsPriceTimesQuantity) {
    cart::State state;
    cart::add(state, "A-100", 9.5, 2);
    cart::add(state, "B-200", 0.25, 4);

    EXPECT_DOUBLE_EQ(cart::total(state), 20.0);
}

TEST(CartPaths, OfferTheAddressesARuleCanUse) {
    cart::State state;
    state.status = "Packing";
    cart::add(state, "A-100", 9.5, 2);
    cart::add(state, "B-200", 0.5, 1);

    // These strings are the whole point of the window: an operator copies one
    // into a rule. If a path here stops matching what the publisher writes,
    // every rule written from this screen is addressed at nothing.
    EXPECT_EQ(value_at(state, "items_.length"), "2");
    EXPECT_EQ(value_at(state, "unit_count"), "3");
    EXPECT_EQ(value_at(state, "status"), "Packing");
    EXPECT_EQ(value_at(state, "items_[0].sku"), "A-100");
}

TEST(CartPaths, StayUsableOnAnEmptyCart) {
    // The window shows these before anything has been added, so an empty cart
    // must not produce a path addressing an element that is not there.
    const cart::State state;
    const auto paths = cart::rule_paths(state);

    EXPECT_EQ(value_at(state, "items_.length"), "0");
    EXPECT_TRUE(std::ranges::none_of(paths, [](const auto& entry) {
        return entry.first.find('[') != std::string::npos;
    })) << "an empty cart has no items_[0] to address";
}
