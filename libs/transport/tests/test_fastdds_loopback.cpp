#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "lm/transport/fast_dds_transport.hpp"

using namespace lm::core;
using namespace lm::transport;
using namespace std::chrono_literals;

namespace {

/// Domain 42 keeps this test off the default domain other tooling might use.
DdsConfig loopback_config() {
    DdsConfig config;
    config.domain_id = 42;
    return config;
}

/// Polls until the predicate holds or the timeout expires. Returns whether it held.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return predicate();
}

}  // namespace

TEST(FastDdsLoopback, ServerDiscoversAnAnnouncingClient) {
    const auto server = make_dds_server(loopback_config());

    std::atomic<bool> seen{false};
    server->on_announce([&](const ClientAnnounce& message) {
        if (message.host_id == "LOOPBACK-PC") {
            seen = true;
        }
    });

    const auto client = make_dds_client(loopback_config());
    ASSERT_TRUE(wait_for([&] { return client->state() == ConnectionState::Connected; }));

    ClientAnnounce announce;
    announce.host_id = "LOOPBACK-PC";
    announce.agent_version = "0.1.0";
    announce.capabilities = platform_capabilities().raw();
    client->publish_announce(announce);

    EXPECT_TRUE(wait_for([&] { return seen.load(); }));
}

TEST(FastDdsLoopback, ResourceSamplesReachTheServer) {
    const auto server = make_dds_server(loopback_config());

    std::atomic<int> received{0};
    server->on_resources([&](const ResourceSampleMessage&) { ++received; });

    const auto client = make_dds_client(loopback_config());
    ASSERT_TRUE(wait_for([&] { return client->state() == ConnectionState::Connected; }));

    ResourceSampleMessage sample;
    sample.host_id = "LOOPBACK-PC";
    sample.sample.cpu_percent = 12.5;
    client->publish_resources(sample);

    EXPECT_TRUE(wait_for([&] { return received.load() > 0; }));
}

TEST(FastDdsLoopback, LateJoiningClientReceivesTheRetainedBundle) {
    const auto server = make_dds_server(loopback_config());

    TemplateBundleMessage bundle;
    bundle.revision = 5;
    bundle.hash = "abc";
    bundle.json = R"({"revision":5,"hash":"abc","baseline":{"name":"b","rules":[]},
                      "templates":[],"assignments":{}})";
    server->publish_bundle(bundle);

    // Client created after the publish — TransientLocal must still deliver it.
    const auto client = make_dds_client(loopback_config());

    std::atomic<std::uint64_t> revision{0};
    client->on_bundle([&](const TemplateBundleMessage& message) { revision = message.revision; });

    EXPECT_TRUE(wait_for([&] { return revision.load() == 5u; }));
}
