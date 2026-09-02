#pragma once

#include <cstdint>
#include <string>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"
#include "lm/core/script.hpp"

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

/// Topic: ScriptCommand. Reliable, **Volatile**, keyed by host_id.
///
/// Volatile is not an oversight and must not be "fixed" to TransientLocal. Every
/// other topic here carries state, where a late joiner should receive the last
/// value. A command is an event: with transient-local durability a client that
/// restarts would receive and execute whatever it missed, so rebooting a machine
/// could silently re-run last week's uninstall.
struct ScriptCommand {
    core::HostId host_id;
    /// Correlates the result back to its run. The client also remembers the
    /// ones it has executed, so a redelivered sample runs once.
    std::string run_id;
    /// For the logs and the audit trail; "(custom script)" when typed in.
    std::string script_name;
    /// The full text. The server always sends the body, never a name -- the
    /// client never reads the share and needs no access to it.
    std::string script_body;
    std::uint32_t timeout_seconds = 120;
    friend bool operator==(const ScriptCommand&, const ScriptCommand&) = default;
};

/// Topic: ScriptResult. Reliable, Volatile, keyed by host_id.
struct ScriptResultMessage {
    core::HostId host_id;
    std::string run_id;
    core::ScriptStatus status = core::ScriptStatus::Error;
    /// Set when status is Refused: not enrolled, already running, no shell.
    std::string refusal_reason;
    std::int32_t exit_code = 0;
    /// Whether the script wrote an LM-RESULT line at all. Kept as a separate
    /// flag rather than a sentinel in reported_ok, so "said nothing" stays
    /// distinguishable from "said it failed" on the wire as well as in core.
    bool has_reported = false;
    bool reported_ok = false;
    std::string reported_message;
    std::string stdout_text;
    std::string stderr_text;
    std::uint64_t duration_ms = 0;
    friend bool operator==(const ScriptResultMessage&, const ScriptResultMessage&) = default;
};

enum class ConnectionState { Disconnected, Connected };

}  // namespace lm::transport
