#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lm::core {

/// Why a path did not resolve. The distinction matters to the caller: a path
/// that is malformed is an authoring mistake, while a path that is well-formed
/// but finds nothing is a fact about the data — and those deserve different
/// words on a wall display.
enum class PathError {
    Malformed,   ///< the path itself is not valid: "items_[", "a..b", trailing '.'
    NoSuchField  ///< well-formed, but the document has nothing there
};

struct PathFailure {
    PathError kind = PathError::NoSuchField;
    /// Ready to show: "no field \"itmes_\" in the sample", "index 4 is past the
    /// end of \"items_\" (2 elements)".
    std::string message;
    friend bool operator==(const PathFailure&, const PathFailure&) = default;
};

/// Reads one value out of a JSON document by path.
///
/// The grammar is deliberately tiny — this is an address, not a query language:
///
///     name                a field
///     a.b.c               nested fields
///     items_[0]           a sequence element
///     items_[0].price     both
///     items_.length       how many elements (also valid on a string or object)
///
/// `length` is a projection, not a function call, and it is only recognised as
/// the *final* segment. That keeps it unambiguous against a field genuinely
/// named `length`, which is addressed as any other field would be as long as
/// something follows it.
///
/// Pure: no I/O, no DDS, no knowledge of where the document came from. That is
/// what lets every path case be tested against a JSON literal.
[[nodiscard]] std::expected<nlohmann::json, PathFailure> resolve_path(const nlohmann::json& document,
                                                                      std::string_view path);

}  // namespace lm::core
