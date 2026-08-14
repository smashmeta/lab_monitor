#include "lm/core/template_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

namespace lm::core {
namespace {

/// Lowercase alphanumerics, every other run of characters collapsed to a single
/// '-'. Ids travel on the wire and turn up in log lines, so they stay in the
/// conservative character set rather than carrying paths and spaces along.
std::string slugify(std::string_view text) {
    std::string slug;
    slug.reserve(text.size());
    for (const char raw : text) {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0) {
            slug.push_back(static_cast<char>(std::tolower(ch)));
        } else if (!slug.empty() && slug.back() != '-') {
            slug.push_back('-');
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    return slug;
}

std::string_view prefix_of(RuleKind kind) {
    switch (kind) {
        case RuleKind::Process:  return "process";
        case RuleKind::Service:  return "service";
        case RuleKind::Registry: return "registry";
    }
    return "rule";
}

/// Whatever an operator would recognise the rule by.
std::string target_of(const Rule& rule) {
    if (const auto* process = std::get_if<ProcessRule>(&rule.payload)) {
        return process->executable;
    }
    if (const auto* service = std::get_if<ServiceRule>(&rule.payload)) {
        return service->service_name;
    }

    const auto& registry = std::get<RegistryRule>(rule.payload);
    if (!registry.value_name.empty()) {
        return registry.value_name;
    }
    // A rule that only asserts a key exists has no value name, so the leaf of
    // the path is the closest thing to a name it has.
    const auto leaf = registry.key_path.find_last_of("\\/");
    return leaf == std::string::npos ? registry.key_path : registry.key_path.substr(leaf + 1);
}

RuleId unique_id(const std::unordered_set<std::string>& taken, const Rule& rule) {
    std::string base{prefix_of(kind_of(rule))};
    const std::string target = slugify(target_of(rule));
    if (!target.empty()) {
        base += '-';
        base += target;
    }

    if (!taken.contains(base)) {
        return base;
    }
    // Starts at 2 so the pair reads as "chrome-exe" and "chrome-exe-2" rather
    // than leaving an unexplained gap at 1.
    for (int suffix = 2;; ++suffix) {
        std::string candidate = base + '-' + std::to_string(suffix);
        if (!taken.contains(candidate)) {
            return candidate;
        }
    }
}

std::unordered_set<std::string> all_ids(const TemplateBundle& bundle) {
    std::unordered_set<std::string> ids;
    for (const Rule& rule : bundle.baseline.rules) {
        ids.insert(rule.id);
    }
    for (const Template& tmpl : bundle.templates) {
        for (const Rule& rule : tmpl.rules) {
            ids.insert(rule.id);
        }
    }
    return ids;
}

}  // namespace

std::vector<const Rule*> rules_for(const TemplateBundle& bundle, const HostId& host_id) {
    std::vector<const Rule*> result;
    std::unordered_set<std::string> seen;

    const auto append = [&](const Template& tmpl) {
        for (const Rule& rule : tmpl.rules) {
            if (seen.insert(rule.id).second) {
                result.push_back(&rule);
            }
        }
    };

    append(bundle.baseline);

    const auto assignment = bundle.assignments.find(host_id);
    if (assignment == bundle.assignments.end()) {
        return result;
    }

    for (const std::string& name : assignment->second) {
        const auto tmpl = std::ranges::find(bundle.templates, name, &Template::name);
        if (tmpl != bundle.templates.end()) {
            append(*tmpl);
        }
    }

    return result;
}

RuleId make_rule_id(const TemplateBundle& bundle, const Rule& rule) {
    return unique_id(all_ids(bundle), rule);
}

std::vector<std::pair<RuleId, RuleId>> deduplicate_rule_ids(TemplateBundle& bundle) {
    std::vector<std::pair<RuleId, RuleId>> renames;
    std::unordered_set<std::string> seen;

    const auto repair = [&](Template& tmpl) {
        for (Rule& rule : tmpl.rules) {
            if (!rule.id.empty() && seen.insert(rule.id).second) {
                continue;  // first holder of a non-empty id: it keeps it
            }
            RuleId replacement = unique_id(seen, rule);
            seen.insert(replacement);
            renames.emplace_back(rule.id, replacement);
            rule.id = std::move(replacement);
        }
    };

    // Baseline first, matching rules_for()'s own order, so the rule that wins
    // there is the rule that keeps its id here.
    repair(bundle.baseline);
    for (Template& tmpl : bundle.templates) {
        repair(tmpl);
    }

    return renames;
}

}  // namespace lm::core
