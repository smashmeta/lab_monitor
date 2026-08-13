#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "lm/core/types.hpp"

namespace lm::core {

enum class HostState { Missing, Offline, Unexpected, Online };

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
    std::size_t stale = 0;
    friend bool operator==(const FleetCounts&, const FleetCounts&) = default;
};

struct FleetView {
    /// Sorted most-urgent-first: Missing, Offline, Unexpected, Online; then by host id.
    std::vector<FleetEntry> entries;
    FleetCounts counts;
    friend bool operator==(const FleetView&, const FleetView&) = default;
};

struct ReconcileOptions {
    /// Matches the DDS Liveliness lease on the ClientAnnounce topic.
    std::chrono::milliseconds liveliness_lease = std::chrono::seconds{10};
    std::uint64_t current_revision = 0;
};

/// Pure: no I/O and no clock — `now` is supplied by the caller.
[[nodiscard]] FleetView reconcile(const std::vector<ExpectedHost>& expected,
                                  const std::vector<DiscoveredClient>& discovered, TimePoint now,
                                  ReconcileOptions options);

[[nodiscard]] std::string to_string(HostState state);

}  // namespace lm::core
