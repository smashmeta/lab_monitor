#include "lm/core/json_path.hpp"

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

namespace lm::core {
namespace {

/// One step of a path: a named field, or an index into a sequence.
struct Segment {
    enum class Kind { Field, Index, AnyIndex } kind = Kind::Field;
    std::string field;
    std::size_t index = 0;
};

struct Parsed {
    std::vector<Segment> segments;
    /// True when the path ended in `.length`, which is a projection over
    /// whatever the preceding segments addressed rather than a step of its own.
    bool length_projection = false;
};

PathFailure malformed(std::string message) {
    return PathFailure{PathError::Malformed, std::move(message)};
}

PathFailure missing(std::string message) {
    return PathFailure{PathError::NoSuchField, std::move(message)};
}

/// Splits a path into segments. Total: every malformed input produces a
/// failure, never an exception, because the input is whatever an operator
/// typed into a rule.
std::expected<Parsed, PathFailure> parse(std::string_view path) {
    Parsed parsed;
    std::size_t i = 0;

    while (i < path.size()) {
        if (path[i] == '[') {
            // An index has to follow something -- "[0]" on its own addresses
            // nothing, and "a[0][1]" is two indices in a row, which is fine.
            if (parsed.segments.empty()) {
                return std::unexpected(malformed("a path cannot start with '['"));
            }
            const std::size_t close = path.find(']', i);
            if (close == std::string_view::npos) {
                return std::unexpected(malformed("unclosed '[' in the path"));
            }
            const std::string_view digits = path.substr(i + 1, close - i - 1);
            if (digits.empty()) {
                return std::unexpected(malformed("empty index in the path"));
            }
            if (digits == "*") {
                // Every element rather than one. Checked before the digit loop
                // because '*' is not a digit and would otherwise be reported as
                // a bad index -- which is exactly the message somebody reaching
                // for a wildcard must not get.
                parsed.segments.push_back(Segment{Segment::Kind::AnyIndex, {}, 0});
            } else {
                std::size_t index = 0;
                for (const char c : digits) {
                    if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                        return std::unexpected(malformed("index \"" + std::string(digits) +
                                                         "\" is not a number or \"*\""));
                    }
                    index = index * 10 + static_cast<std::size_t>(c - '0');
                }
                parsed.segments.push_back(Segment{Segment::Kind::Index, {}, index});
            }
            i = close + 1;
            // After a ']' only another index or a '.' may follow.
            if (i < path.size() && path[i] != '.' && path[i] != '[') {
                return std::unexpected(malformed("expected '.' or '[' after ']' in the path"));
            }
            if (i < path.size() && path[i] == '.') {
                ++i;
                if (i == path.size()) {
                    return std::unexpected(malformed("the path ends with '.'"));
                }
            }
            continue;
        }

        const std::size_t next = path.find_first_of(".[", i);
        const std::string_view name = path.substr(i, next == std::string_view::npos ? next : next - i);
        if (name.empty()) {
            return std::unexpected(malformed("empty field name in the path"));
        }
        parsed.segments.push_back(Segment{Segment::Kind::Field, std::string(name), 0});
        if (next == std::string_view::npos) {
            break;
        }
        i = next;
        if (path[i] == '.') {
            ++i;
            if (i == path.size()) {
                return std::unexpected(malformed("the path ends with '.'"));
            }
        }
    }

    // Only in final position, so a field genuinely named "length" stays
    // addressable as long as the path continues past it.
    if (!parsed.segments.empty() && parsed.segments.back().kind == Segment::Kind::Field &&
        parsed.segments.back().field == "length") {
        parsed.segments.pop_back();
        parsed.length_projection = true;
    }

    return parsed;
}

std::string quoted_name(const std::string& text) { return "\"" + text + "\""; }

/// Walks one already-addressed node through the remaining segments.
///
/// Recursive only where it has to be: a `[*]` is the one segment that turns a
/// single node into several, so the walk forks there and is a plain loop
/// everywhere else.
std::expected<void, PathFailure> walk(const nlohmann::json& node, const Parsed& parsed,
                                      std::size_t from, std::string walked,
                                      std::vector<nlohmann::json>& out);

/// Applies `.length` and appends the result, or fails saying why it cannot.
std::expected<void, PathFailure> append_leaf(const nlohmann::json& node, const Parsed& parsed,
                                             const std::string& walked,
                                             std::vector<nlohmann::json>& out) {
    if (!parsed.length_projection) {
        out.push_back(node);
        return {};
    }
    // Strings are handled separately because nlohmann::json::size() reports 1
    // for any non-container -- a string included. "Ready".length must be 5,
    // not 1, which is what a person means by the length of a string.
    if (node.is_string()) {
        out.emplace_back(node.get_ref<const std::string&>().size());
        return {};
    }
    if (!node.is_array() && !node.is_object()) {
        return std::unexpected(missing(quoted_name(walked) +
                                       " has no length: it is neither a sequence, an object "
                                       "nor a string"));
    }
    out.emplace_back(node.size());
    return {};
}

std::expected<void, PathFailure> walk(const nlohmann::json& node, const Parsed& parsed,
                                      std::size_t from, std::string walked,
                                      std::vector<nlohmann::json>& out) {
    const nlohmann::json* current = &node;

    for (std::size_t i = from; i < parsed.segments.size(); ++i) {
        const Segment& segment = parsed.segments[i];

        if (segment.kind == Segment::Kind::Field) {
            if (!current->is_object()) {
                return std::unexpected(missing(
                    walked.empty() ? "the sample is not an object, so it has no field " +
                                         quoted_name(segment.field)
                                   : quoted_name(walked) + " is not an object, so it has no field " +
                                         quoted_name(segment.field)));
            }
            const auto found = current->find(segment.field);
            if (found == current->end()) {
                return std::unexpected(missing("no field " + quoted_name(segment.field) +
                                               (walked.empty() ? " in the sample"
                                                               : " in " + quoted_name(walked))));
            }
            current = &*found;
            walked = walked.empty() ? segment.field : walked + "." + segment.field;
            continue;
        }

        if (segment.kind == Segment::Kind::Index) {
            if (!current->is_array()) {
                return std::unexpected(
                    missing(quoted_name(walked) + " is not a sequence, so it cannot be indexed"));
            }
            if (segment.index >= current->size()) {
                // The count is the useful half: "index 4 is past the end" alone
                // still leaves the reader wondering how many there are.
                return std::unexpected(missing("index " + std::to_string(segment.index) +
                                               " is past the end of " + quoted_name(walked) + " (" +
                                               std::to_string(current->size()) + " elements)"));
            }
            current = &(*current)[segment.index];
            walked += "[" + std::to_string(segment.index) + "]";
            continue;
        }

        // AnyIndex: fork, one branch per element, and stop looping here --
        // each branch finishes the rest of the path for itself.
        if (!current->is_array()) {
            return std::unexpected(
                missing(quoted_name(walked) + " is not a sequence, so it cannot be indexed"));
        }
        // An empty sequence addresses nothing, and that is not a failure: it is
        // the answer "there are no elements", which a rule turns into a Fail
        // rather than an Error.
        std::optional<PathFailure> first_failure;
        const std::size_t before = out.size();
        for (std::size_t element = 0; element < current->size(); ++element) {
            const std::string branch = walked + "[" + std::to_string(element) + "]";
            auto stepped = walk((*current)[element], parsed, i + 1, branch, out);
            if (!stepped.has_value() && !first_failure.has_value()) {
                first_failure = stepped.error();
            }
        }
        // Only when *nothing* came back. One element missing a field while its
        // siblings have it is a heterogeneous sequence, which is data; every
        // element missing it is a path that addresses nothing, which is almost
        // always a typo and deserves to say so.
        if (out.size() == before && first_failure.has_value()) {
            return std::unexpected(*first_failure);
        }
        return {};
    }

    return append_leaf(*current, parsed, walked, out);
}

}  // namespace

std::expected<std::vector<nlohmann::json>, PathFailure> resolve_all(const nlohmann::json& document,
                                                                    std::string_view path) {
    if (path.empty()) {
        return std::vector<nlohmann::json>{document};
    }

    const auto parsed = parse(path);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }

    std::vector<nlohmann::json> values;
    auto walked = walk(document, *parsed, 0, std::string{}, values);
    if (!walked.has_value()) {
        return std::unexpected(walked.error());
    }
    return values;
}

std::expected<nlohmann::json, PathFailure> resolve_path(const nlohmann::json& document,
                                                        std::string_view path) {
    auto values = resolve_all(document, path);
    if (!values.has_value()) {
        return std::unexpected(values.error());
    }
    if (values->empty()) {
        return std::unexpected(missing("the path addressed no value"));
    }
    if (values->size() > 1) {
        return std::unexpected(malformed("the path addresses " + std::to_string(values->size()) +
                                         " values; it needs a rule that reads them all"));
    }
    return std::move(values->front());
}

}  // namespace lm::core
