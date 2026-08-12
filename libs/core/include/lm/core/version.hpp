#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lm::core {

struct Version {
    std::vector<int> parts;
    friend bool operator==(const Version&, const Version&) = default;
};

enum class ComparisonOp { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

struct VersionConstraint {
    ComparisonOp op = ComparisonOp::GreaterEqual;
    Version value;
    friend bool operator==(const VersionConstraint&, const VersionConstraint&) = default;
};

/// Parses a dotted numeric version. Returns nullopt for empty input, empty
/// components, negative numbers, or any non-digit character.
[[nodiscard]] std::optional<Version> parse_version(std::string_view text);

/// Three-way comparison. Missing trailing components are treated as zero, so
/// "1.2" and "1.2.0" compare equal.
[[nodiscard]] int compare(const Version& lhs, const Version& rhs);

[[nodiscard]] bool satisfies(const Version& actual, const VersionConstraint& constraint);

[[nodiscard]] std::string to_string(const Version& version);

[[nodiscard]] std::string to_string(ComparisonOp op);
[[nodiscard]] std::optional<ComparisonOp> parse_comparison_op(std::string_view text);

}  // namespace lm::core
