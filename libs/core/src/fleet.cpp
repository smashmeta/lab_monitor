#include "lm/core/fleet.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace lm::core {
namespace {

/// Lower sorts first. Not the declaration order of HostState: Paused was
/// appended to that enum but belongs here between Unexpected and Online.
/// Somebody chose to pause a machine, so it is not an alarm -- but it is not
/// being checked either, and it should not sit among the healthy ones where it
/// can be left paused and forgotten.
int urgency(HostState state) {
    switch (state) {
        case HostState::Missing:    return 0;
        case HostState::Offline:    return 1;
        case HostState::Unexpected: return 2;
        case HostState::Paused:     return 3;
        case HostState::Online:     return 4;
    }
    return 5;
}

}  // namespace

FleetView reconcile(const std::vector<ExpectedHost>& expected,
                    const std::vector<DiscoveredClient>& discovered, TimePoint now,
                    ReconcileOptions options) {
    // Collapse duplicate sightings, keeping the most recent per host.
    std::map<HostId, DiscoveredClient> latest;
    for (const DiscoveredClient& client : discovered) {
        const auto existing = latest.find(client.host_id);
        if (existing == latest.end() || client.last_seen > existing->second.last_seen) {
            latest.insert_or_assign(client.host_id, client);
        }
    }

    const auto within_lease = [&](TimePoint last_seen) {
        if (last_seen >= now) {
            return true;  // clock skew from the client — treat as current
        }
        return (now - last_seen) <= options.liveliness_lease;
    };

    FleetView view;

    // Every host in `expected` that also appears in `latest` is classified by
    // the loop below, so the unexpected pass just has to skip anything expected.
    // Testing membership against a set replaces the old parallel `accounted`
    // vector and its linear scan per candidate, which made this O(n^2).
    std::set<HostId> expected_ids;
    for (const ExpectedHost& host : expected) {
        expected_ids.insert(host.host_id);
    }

    for (const ExpectedHost& host : expected) {
        FleetEntry entry;
        entry.host_id = host.host_id;
        entry.address = host.address;

        const auto seen = latest.find(host.host_id);
        if (seen == latest.end()) {
            entry.state = HostState::Missing;
        } else {
            entry.last_seen = seen->second.last_seen;
            entry.caps = seen->second.caps;
            entry.stale = seen->second.applied_revision != options.current_revision;
            // Offline wins over Paused once the lease has expired. "It said it
            // was paused" is a claim about intent, not about liveness, and a
            // machine we have stopped hearing from has not made a liveness
            // claim at all -- whatever the last announce happened to say.
            if (!within_lease(seen->second.last_seen)) {
                entry.state = HostState::Offline;
            } else {
                entry.state = seen->second.paused ? HostState::Paused : HostState::Online;
            }
        }
        view.entries.push_back(std::move(entry));
    }

    for (const auto& [host_id, client] : latest) {
        if (expected_ids.contains(host_id)) {
            continue;
        }
        FleetEntry entry;
        entry.host_id = host_id;
        entry.state = HostState::Unexpected;
        entry.last_seen = client.last_seen;
        entry.caps = client.caps;
        entry.stale = client.applied_revision != options.current_revision;
        view.entries.push_back(std::move(entry));
    }

    std::ranges::sort(view.entries,
              [](const FleetEntry& a, const FleetEntry& b) {
                  const int ua = urgency(a.state);
                  const int ub = urgency(b.state);
                  return ua != ub ? ua < ub : a.host_id < b.host_id;
              });

    for (const FleetEntry& entry : view.entries) {
        switch (entry.state) {
            case HostState::Online:     ++view.counts.online; break;
            case HostState::Offline:    ++view.counts.offline; break;
            case HostState::Missing:    ++view.counts.missing; break;
            case HostState::Unexpected: ++view.counts.unexpected; break;
            case HostState::Paused:     ++view.counts.paused; break;
        }
        if (entry.stale) {
            ++view.counts.stale;
        }
    }

    return view;
}

std::string to_string(HostState state) {
    switch (state) {
        case HostState::Online:     return "Online";
        case HostState::Offline:    return "Offline";
        case HostState::Missing:    return "Missing";
        case HostState::Unexpected: return "Unexpected";
        case HostState::Paused:     return "Paused";
    }
    return "Unknown";
}

}  // namespace lm::core
