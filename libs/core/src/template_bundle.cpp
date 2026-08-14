#include "lm/core/template_bundle.hpp"

#include <algorithm>
#include <unordered_set>

namespace lm::core {

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

}  // namespace lm::core
