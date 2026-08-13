#pragma once

#include <map>
#include <vector>

#include "lm/core/fleet.hpp"

namespace lm::core {

/// Pure bookkeeping of clients the server has heard from. No I/O and no clock:
/// timestamps are supplied by the caller, so this is fully testable.
class ClientRegistry {
public:
    void record_announce(const HostId& host_id, Capabilities caps, TimePoint seen_at);
    void record_sample(const HostId& host_id, TimePoint seen_at);
    void record_report(const HostId& host_id, std::uint64_t applied_revision, TimePoint seen_at);
    void mark_lost(const HostId& host_id);

    /// Ordered by host id; feeds straight into reconcile().
    [[nodiscard]] std::vector<DiscoveredClient> snapshot() const;

private:
    DiscoveredClient& touch(const HostId& host_id, TimePoint seen_at);

    std::map<HostId, DiscoveredClient> clients_;
};

}  // namespace lm::core
