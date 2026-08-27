#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lm/core/types.hpp"

namespace lm::core {

/// Appended to rather than reordered: Paused is last because this enum is
/// switched on in a dozen places and never serialised, so the numeric values
/// are nobody s business. Display order is urgency(), not declaration order.
enum class HostState { Missing, Offline, Unexpected, Online, Paused };

struct ExpectedHost {
    HostId host_id;
    /// Hostname or IP address, as configured by the operator. Informational.
    std::string address;
    friend bool operator==(const ExpectedHost&, const ExpectedHost&) = default;
};

struct DiscoveredClient {
    HostId host_id;
    TimePoint last_seen{};
    Capabilities caps;
    std::uint64_t applied_revision = 0;
    /// Last thing this client said about whether the operator has paused it.
    /// Carried on the announce, which keeps arriving while paused -- see
    /// transport::ClientAnnounce.
    bool paused = false;
    friend bool operator==(const DiscoveredClient&, const DiscoveredClient&) = default;
};

struct FleetEntry {
    HostId host_id;
    std::string address;
    HostState state = HostState::Missing;
    std::optional<TimePoint> last_seen;
    /// Reporting, but on an older template revision than the server's current one.
    bool stale = false;
    Capabilities caps;
    friend bool operator==(const FleetEntry&, const FleetEntry&) = default;
};

struct FleetCounts {
    std::size_t online = 0;
    std::size_t offline = 0;
    std::size_t missing = 0;
    std::size_t unexpected = 0;
    std::size_t paused = 0;
    std::size_t stale = 0;
    friend bool operator==(const FleetCounts&, const FleetCounts&) = default;
};

struct FleetView {
    /// Sorted most-urgent-first: Missing, Offline, Unexpected, Paused, Online;
    /// then by host id.
    std::vector<FleetEntry> entries;
    FleetCounts counts;
    friend bool operator==(const FleetView&, const FleetView&) = default;
};

struct ReconcileOptions {
    /// Three announce intervals, not one.
    ///
    /// The client announces every 10 s, and a lease of the same 10 s is a
    /// knife-edge: one jittered or dropped heartbeat flips a healthy machine to
    /// Offline. That was always fragile, and it got worse when the announce
    /// became the only carrier of the paused flag -- a missed beat would drop
    /// the pause reading too, so a paused machine would flicker to Offline and
    /// back. The cost is that a genuinely dead host takes up to 30 s rather
    /// than 10 s to be called Offline.
    std::chrono::milliseconds liveliness_lease = std::chrono::seconds{30};
    std::uint64_t current_revision = 0;
};

/// Pure: no I/O and no clock — `now` is supplied by the caller.
[[nodiscard]] FleetView reconcile(const std::vector<ExpectedHost>& expected,
                                  const std::vector<DiscoveredClient>& discovered, TimePoint now,
                                  ReconcileOptions options);

[[nodiscard]] std::string to_string(HostState state);

}  // namespace lm::core
