#include "lm/core/version.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace lm::core {
namespace {

int component_at(const Version& v, std::size_t index) {
    return index < v.parts.size() ? v.parts[index] : 0;
}

}  // namespace

std::optional<Version> parse_version(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }

    Version result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t dot = text.find('.', start);
        const std::string_view part =
            text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);

        if (part.empty() || !std::ranges::all_of(part, [](unsigned char c) {
                return std::isdigit(c) != 0;
            })) {
            return std::nullopt;
        }

        int value = 0;
        const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (ec != std::errc{} || ptr != part.data() + part.size()) {
            return std::nullopt;
        }
        result.parts.push_back(value);

        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }

    return result;
}

int compare(const Version& lhs, const Version& rhs) {
    const std::size_t count = std::max(lhs.parts.size(), rhs.parts.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int a = component_at(lhs, i);
        const int b = component_at(rhs, i);
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    return 0;
}

bool satisfies(const Version& actual, const VersionConstraint& constraint) {
    const int result = compare(actual, constraint.value);
    switch (constraint.op) {
        case ComparisonOp::Equal:        return result == 0;
        case ComparisonOp::NotEqual:     return result != 0;
        case ComparisonOp::Less:         return result < 0;
        case ComparisonOp::LessEqual:    return result <= 0;
        case ComparisonOp::Greater:      return result > 0;
        case ComparisonOp::GreaterEqual: return result >= 0;
    }
    return false;
}

std::string to_string(const Version& version) {
    std::string out;
    for (std::size_t i = 0; i < version.parts.size(); ++i) {
        if (i > 0) {
            out += '.';
        }
        out += std::to_string(version.parts[i]);
    }
    return out;
}

std::string to_string(ComparisonOp op) {
    switch (op) {
        case ComparisonOp::Equal:        return "==";
        case ComparisonOp::NotEqual:     return "!=";
        case ComparisonOp::Less:         return "<";
        case ComparisonOp::LessEqual:    return "<=";
        case ComparisonOp::Greater:      return ">";
        case ComparisonOp::GreaterEqual: return ">=";
    }
    return "==";
}

std::optional<ComparisonOp> parse_comparison_op(std::string_view text) {
    if (text == "==") return ComparisonOp::Equal;
    if (text == "!=") return ComparisonOp::NotEqual;
    if (text == "<")  return ComparisonOp::Less;
    if (text == "<=") return ComparisonOp::LessEqual;
    if (text == ">")  return ComparisonOp::Greater;
    if (text == ">=") return ComparisonOp::GreaterEqual;
    return std::nullopt;
}

}  // namespace lm::core
