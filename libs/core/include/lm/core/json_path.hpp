#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

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
///     items_[*].sku       that field of *every* element
///     items_.length       how many elements (also valid on a string or object)
///
/// `length` is a projection, not a function call, and it is only recognised as
/// the *final* segment. That keeps it unambiguous against a field genuinely
/// named `length`, which is addressed as any other field would be as long as
/// something follows it.
///
/// `[*]` is still an address, not a query: it names every element rather than
/// one, and what to *do* with the resulting values is the caller's business —
/// a rule asks whether any of them matches. `*` was never a valid index, so no
/// path that parsed before means anything different now. The spelling leaves
/// room for `[all]` and `[none]` later without disturbing anything already
/// saved in a bundle.
///
/// Pure: no I/O, no DDS, no knowledge of where the document came from. That is
/// what lets every path case be tested against a JSON literal.
/// Every value the path addresses, in document order.
///
/// One element for a path with no `[*]`, so this is the general form and
/// resolve_path() is the special case. Zero elements is a *success*: a `[*]`
/// over an empty sequence addresses nothing, which is a fact about the data
/// ("no item has that sku") rather than a broken rule.
///
/// When `[*]` walks a non-empty sequence and the remainder of the path fails
/// for *every* element, the first element's failure is returned — that is the
/// message a typo like `items_[*].skew` needs. Elements that fail while others
/// succeed are skipped instead: a sequence whose members differ is data, not
/// an authoring mistake.
[[nodiscard]] std::expected<std::vector<nlohmann::json>, PathFailure> resolve_all(
    const nlohmann::json& document, std::string_view path);

/// The single value the path addresses.
///
/// Fails when the path addresses none, and when it addresses more than one —
/// there is no single answer to return, and silently picking the first would
/// make `items_[*].sku` look like it worked.
[[nodiscard]] std::expected<nlohmann::json, PathFailure> resolve_path(const nlohmann::json& document,
                                                                      std::string_view path);

}  // namespace lm::core
