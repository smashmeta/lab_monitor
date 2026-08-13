#pragma once

#include <cstdint>
#include <map>
#include <string>
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

}  // namespace lm::core
