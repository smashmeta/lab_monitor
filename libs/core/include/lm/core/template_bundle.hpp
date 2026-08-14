#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "lm/core/rule.hpp"

namespace lm::core {

struct Template {
    std::string name;
    std::vector<Rule> rules;
    friend bool operator==(const Template&, const Template&) = default;
};

struct TemplateBundle {
    std::uint64_t revision = 0;
    std::string hash;
    Template baseline;
    std::vector<Template> templates;
    /// Host id -> names of assigned templates.
    std::map<HostId, std::vector<std::string>> assignments;
    friend bool operator==(const TemplateBundle&, const TemplateBundle&) = default;
};

/// Returns the baseline rules plus the rules of every template assigned to the
/// host, deduplicated by rule id. Assignments naming a template that does not
/// exist are ignored. Pointers remain valid as long as the bundle does.
[[nodiscard]] std::vector<const Rule*> rules_for(const TemplateBundle& bundle,
                                                 const HostId& host_id);

/// A readable id for a new rule, unique across the whole bundle.
///
/// Ids are the join key between a rule and the CheckResult reported for it, and
/// rules_for() merges the baseline with every assigned template keeping only the
/// *first* rule per id -- so reusing one silently drops a rule from evaluation
/// and makes any lookup by id ambiguous. Generating the id from the rule itself
/// means nobody has to keep a mental ledger of which strings are already taken.
///
/// Uniqueness is bundle-wide, not per-template: templates are combined per host,
/// so an id free in this template but used in another is not free.
[[nodiscard]] RuleId make_rule_id(const TemplateBundle& bundle, const Rule& rule);

/// Repairs a bundle whose ids were typed by hand: gives every rule with a
/// duplicate or empty id a freshly generated one, and returns old -> new for
/// each. The first rule holding an id keeps it, since that is the one
/// rules_for() is already handing to clients.
std::vector<std::pair<RuleId, RuleId>> deduplicate_rule_ids(TemplateBundle& bundle);

}  // namespace lm::core
