#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "lm/transport/messages.hpp"

namespace lm::transport {

[[nodiscard]] std::vector<std::uint8_t> encode(const ClientAnnounce& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const ResourceSampleMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const TemplateBundleMessage& message);
[[nodiscard]] std::vector<std::uint8_t> encode(const ComplianceReportMessage& message);

/// Returns false on truncated or malformed input; never throws and never leaves
/// the output partially populated in a way the caller could mistake for success.
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, ClientAnnounce& out);
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, ResourceSampleMessage& out);
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, TemplateBundleMessage& out);
[[nodiscard]] bool decode(std::span<const std::uint8_t> bytes, ComplianceReportMessage& out);

[[nodiscard]] std::string key_of(const ClientAnnounce& message);
[[nodiscard]] std::string key_of(const ResourceSampleMessage& message);
[[nodiscard]] std::string key_of(const ComplianceReportMessage& message);

}  // namespace lm::transport
