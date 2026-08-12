#include "lm/core/client_registry.hpp"

namespace lm::core {

DiscoveredClient& ClientRegistry::touch(const HostId& host_id, TimePoint seen_at) {
    const auto [it, inserted] = clients_.try_emplace(host_id);
    DiscoveredClient& client = it->second;
    if (inserted) {
        client.host_id = host_id;
        client.last_seen = seen_at;
    } else if (seen_at > client.last_seen) {
        // Advance only forward: an out-of-order (e.g. delayed/reordered)
        // sample must never make a live client look stale by rewinding
        // last_seen.
        client.last_seen = seen_at;
    }
    return client;
}

void ClientRegistry::record_announce(const HostId& host_id, Capabilities caps, TimePoint seen_at) {
    DiscoveredClient& client = touch(host_id, seen_at);
    client.caps = caps;
    // Deliberately does not touch applied_revision: a re-announce (e.g. after
    // a client reconnect) must not make an already-compliant client look
    // stale again.
}

void ClientRegistry::record_sample(const HostId& host_id, TimePoint seen_at) { touch(host_id, seen_at); }

void ClientRegistry::record_report(const HostId& host_id, std::uint64_t applied_revision, TimePoint seen_at) {
    DiscoveredClient& client = touch(host_id, seen_at);
    client.applied_revision = applied_revision;
}

void ClientRegistry::mark_lost(const HostId& host_id) { clients_.erase(host_id); }

std::vector<DiscoveredClient> ClientRegistry::snapshot() const {
    // std::map<HostId, ...> already iterates in key (host id) order, so no
    // separate sort is needed here.
    std::vector<DiscoveredClient> result;
    result.reserve(clients_.size());
    for (const auto& [host_id, client] : clients_) {
        (void)host_id;
        result.push_back(client);
    }
    return result;
}

}  // namespace lm::core
