#pragma once

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "lm/core/compliance_fwd.hpp"
#include "lm/core/template_bundle.hpp"

namespace lm::core {

void to_json(nlohmann::json& j, const Version& value);
void from_json(const nlohmann::json& j, Version& value);

void to_json(nlohmann::json& j, const VersionConstraint& value);
void from_json(const nlohmann::json& j, VersionConstraint& value);

void to_json(nlohmann::json& j, const Rule& value);
void from_json(const nlohmann::json& j, Rule& value);

void to_json(nlohmann::json& j, const Template& value);
void from_json(const nlohmann::json& j, Template& value);

void to_json(nlohmann::json& j, const TemplateBundle& value);
void from_json(const nlohmann::json& j, TemplateBundle& value);

[[nodiscard]] std::string serialise_bundle(const TemplateBundle& bundle);

/// Parses a bundle. Returns the parse or validation error message on failure;
/// never throws.
[[nodiscard]] std::expected<TemplateBundle, std::string> parse_bundle(std::string_view text);

/// Stable hash over baseline, templates and assignments. Deliberately excludes
/// revision and hash so a draft can be compared against a published bundle.
[[nodiscard]] std::string content_hash(const TemplateBundle& bundle);

}  // namespace lm::core
