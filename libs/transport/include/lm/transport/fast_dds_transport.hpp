#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "lm/transport/transport.hpp"

namespace lm::transport {

struct DdsConfig {
    int domain_id = 0;
    /// Explicit peers for networks where multicast discovery is blocked.
    /// Empty means rely on default multicast discovery.
    std::vector<std::string> initial_peers;
    std::chrono::milliseconds liveliness_lease = std::chrono::seconds{10};
};

[[nodiscard]] std::unique_ptr<IClientTransport> make_dds_client(const DdsConfig& config);
[[nodiscard]] std::unique_ptr<IServerTransport> make_dds_server(const DdsConfig& config);

}  // namespace lm::transport
