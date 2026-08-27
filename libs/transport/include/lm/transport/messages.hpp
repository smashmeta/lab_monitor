#pragma once

#include <cstdint>
#include <string>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"

namespace lm::transport {

/// Topic: ClientAnnounce. Reliable, TransientLocal, keyed by host_id.
struct ClientAnnounce {
    core::HostId host_id;
    std::string agent_version;
    /// core::Capabilities::raw()
    std::uint32_t capabilities = 0;
    /// The operator has paused reporting on this client.
    ///
    /// It rides the announce rather than stopping it. Pausing suppresses the
    /// resource samples and the compliance reports, so the announce is the only
    /// thing still arriving -- which makes it the one place that can say why the
    /// rest went quiet. A client that stopped announcing too would be
    /// indistinguishable from one that had died, which is exactly the reading
    /// this exists to prevent.
    bool paused = false;
    friend bool operator==(const ClientAnnounce&, const ClientAnnounce&) = default;
};

/// Topic: ResourceSample. BestEffort, Volatile, KeepLast(1), keyed by host_id.
struct ResourceSampleMessage {
    core::HostId host_id;
    core::ResourceSample sample;
    friend bool operator==(const ResourceSampleMessage&, const ResourceSampleMessage&) = default;
};

/// Topic: TemplateBundle. Reliable, TransientLocal, KeepLast(1).
/// The bundle travels as JSON; revision and hash are duplicated in the envelope
/// so a receiver can discard a message it already has without parsing it.
struct TemplateBundleMessage {
    std::uint64_t revision = 0;
    std::string hash;
    std::string json;
    friend bool operator==(const TemplateBundleMessage&, const TemplateBundleMessage&) = default;
};

/// Topic: ComplianceReport. Reliable, TransientLocal, keyed by host_id.
struct ComplianceReportMessage {
    core::ComplianceReport report;
    friend bool operator==(const ComplianceReportMessage&, const ComplianceReportMessage&) = default;
};

enum class ConnectionState { Disconnected, Connected };

}  // namespace lm::transport
