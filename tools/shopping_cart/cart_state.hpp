#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// A stand-in for the sibling application a DDS rule is written against.
///
/// This whole tool exists to be *driven by hand*: add an item, watch a rule on
/// the server go from failing to passing. Keeping the cart itself a plain value
/// type means the interesting part — what a rule can address — is decided here
/// and testable without Qt or a bus.
namespace cart {

struct Item {
    std::string sku;
    double price = 0.0;
    std::int32_t quantity = 1;
    friend bool operator==(const Item&, const Item&) = default;
};

/// What the cart is doing, as free text rather than an enum.
///
/// A rule matches this with `equals` or `containing`, and an operator writing
/// one types the word they can see in the window. An enum would travel the wire
/// as a number and give them nothing to type.
struct State {
    std::vector<Item> items;
    std::string status = "Ready";
    friend bool operator==(const State&, const State&) = default;
};

/// Adds an item, merging into an existing line with the same SKU rather than
/// appending a duplicate — which is what a shopping cart does, and it keeps
/// `items_.length` meaning "distinct products" rather than "clicks".
void add(State& state, const std::string& sku, double price, std::int32_t quantity);

/// True when a line was removed. Case-sensitive: these are SKUs, not names.
bool remove(State& state, const std::string& sku);

/// Sum of quantities — every unit, not every line. `items_.length` counts the
/// lines, so the two differ as soon as anything has a quantity above one, and
/// a rule can ask about either.
[[nodiscard]] std::int32_t unit_count(const State& state);

[[nodiscard]] double total(const State& state);

/// The paths a DDS rule can address, with their current values, exactly as the
/// publisher will write them. Rendered in the window so the person driving this
/// can copy a path straight into a rule instead of guessing at the grammar.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> rule_paths(const State& state);

}  // namespace cart
