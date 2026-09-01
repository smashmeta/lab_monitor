#include "cart_state.hpp"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <utility>

namespace cart {
namespace {

/// Two decimals, and no trailing zeros beyond them: prices read as prices.
/// std::to_string would render 9.5 as "9.500000", which is the same number and
/// a worse thing to copy into a rule.
std::string money(double value) {
    std::ostringstream out;
    out.precision(2);
    out << std::fixed << value;
    return out.str();
}

}  // namespace

void add(State& state, const std::string& sku, double price, std::int32_t quantity) {
    if (sku.empty() || quantity <= 0) {
        return;
    }
    const auto existing = std::ranges::find(state.items, sku, &Item::sku);
    if (existing != state.items.end()) {
        // Merged, and deliberately at the original price: the cart is not a
        // price list, and re-adding a line must not silently re-price it.
        existing->quantity += quantity;
        return;
    }
    state.items.push_back(Item{sku, price, quantity});
}

bool remove(State& state, const std::string& sku) {
    return std::erase_if(state.items, [&](const Item& item) { return item.sku == sku; }) > 0;
}

std::int32_t unit_count(const State& state) {
    return std::accumulate(state.items.begin(), state.items.end(), std::int32_t{0},
                            [](std::int32_t sum, const Item& item) { return sum + item.quantity; });
}

double total(const State& state) {
    return std::accumulate(state.items.begin(), state.items.end(), 0.0,
                            [](double sum, const Item& item) {
                                return sum + item.price * static_cast<double>(item.quantity);
                            });
}

namespace {

/// One field of every line, comma-separated, as the "current value" for a
/// wildcard path. A `[*]` path addresses several values at once, so the pane
/// has to show several -- and showing only the first would make the path look
/// like it meant `items_[0]`.
template <typename Field>
std::string every(const State& state, Field field) {
    if (state.items.empty()) {
        return "(no lines)";
    }
    std::string text;
    for (const Item& item : state.items) {
        if (!text.empty()) {
            text += ", ";
        }
        text += field(item);
    }
    return text;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> rule_paths(const State& state) {
    std::vector<std::pair<std::string, std::string>> paths;
    paths.emplace_back("items_.length", std::to_string(state.items.size()));
    paths.emplace_back("unit_count", std::to_string(unit_count(state)));
    paths.emplace_back("total", money(total(state)));
    paths.emplace_back("status", state.status);

    // Offered whatever the cart holds, unlike the indexed paths below: a rule
    // on items_[*].sku is answerable against an empty cart -- it simply fails,
    // which is the reading an operator wants -- so there is nothing to protect
    // them from here.
    paths.emplace_back("items_[*].sku", every(state, [](const Item& item) { return item.sku; }));
    paths.emplace_back("items_[*].quantity",
                       every(state, [](const Item& item) { return std::to_string(item.quantity); }));

    // Only for lines that exist. Offering items_[0].sku on an empty cart would
    // invite a rule addressed at nothing, which reports Error rather than the
    // failure the operator was aiming for.
    if (!state.items.empty()) {
        paths.emplace_back("items_[0].sku", state.items.front().sku);
        paths.emplace_back("items_[0].quantity", std::to_string(state.items.front().quantity));
    }
    return paths;
}

}  // namespace cart
