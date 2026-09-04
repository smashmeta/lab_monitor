#include <gtest/gtest.h>

#include "lm/core/json.hpp"  // serialise_bundle, parse_bundle, content_hash
#include "lm/transport/in_memory_transport.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

ClientAnnounce announce(HostId id) {
    ClientAnnounce message;
    message.host_id = std::move(id);
    message.agent_version = "0.1.0";
    message.capabilities = platform_capabilities().raw();
    return message;
}

TemplateBundleMessage bundle_message(std::uint64_t revision) {
    TemplateBundle bundle;
    bundle.revision = revision;
    bundle.baseline.name = "baseline";

    TemplateBundleMessage message;
    message.revision = revision;
    message.hash = content_hash(bundle);
    message.json = serialise_bundle(bundle);
    return message;
}

}  // namespace

TEST(InMemoryTransport, ServerReceivesClientAnnouncements) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::vector<HostId> seen;
    server->on_announce([&](const ClientAnnounce& message) { seen.push_back(message.host_id); });

    client->publish_announce(announce("PC-001"));
    EXPECT_EQ(seen, (std::vector<HostId>{"PC-001"}));
}

TEST(InMemoryTransport, ServerReceivesResourceSamples) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::optional<ResourceSampleMessage> received;
    server->on_resources([&](const ResourceSampleMessage& message) { received = message; });

    ResourceSampleMessage sample;
    sample.host_id = "PC-001";
    sample.sample.cpu_percent = 37.5;
    sample.sample.mem_total_bytes = 8ull * 1024 * 1024 * 1024;
    sample.sample.disks.push_back(DiskUsage{"C:\\", 500'000'000'000ull, 120'000'000'000ull});
    client->publish_resources(sample);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->host_id, "PC-001");
    EXPECT_DOUBLE_EQ(received->sample.cpu_percent, 37.5);
    EXPECT_EQ(received->sample.disks.size(), 1u);
}

TEST(InMemoryTransport, ClientReceivesPublishedBundles) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::optional<TemplateBundleMessage> received;
    client->on_bundle([&](const TemplateBundleMessage& message) { received = message; });

    server->publish_bundle(bundle_message(4));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->revision, 4u);
    const auto parsed = parse_bundle(received->json);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    EXPECT_EQ(parsed->revision, 4u);
}

TEST(InMemoryTransport, LateJoiningClientGetsTheCurrentBundle) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    server->publish_bundle(bundle_message(7));

    // Client attaches only after the bundle was published — this models the
    // TransientLocal durability the DDS transport relies on.
    const auto client = make_in_memory_client(bus);

    std::optional<TemplateBundleMessage> received;
    client->on_bundle([&](const TemplateBundleMessage& message) { received = message; });

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->revision, 7u);
}

TEST(InMemoryTransport, ServerReceivesComplianceReports) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::optional<ComplianceReportMessage> received;
    server->on_report([&](const ComplianceReportMessage& message) { received = message; });

    ComplianceReportMessage message;
    message.report.host_id = "PC-001";
    message.report.applied_revision = 4;
    message.report.results.push_back(CheckResult{"r1", CheckStatus::Pass, "running", ""});
    client->publish_report(message);

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->report.host_id, "PC-001");
    EXPECT_EQ(received->report.applied_revision, 4u);
    ASSERT_EQ(received->report.results.size(), 1u);
    EXPECT_EQ(received->report.results.front().status, CheckStatus::Pass);
    // Whole-object equality closes the gap the narrower assertions above leave:
    // it also covers rule_id, observed and message, which a codec could drop
    // or corrupt without failing any of the field checks above.
    EXPECT_EQ(received->report, message.report);
}

TEST(InMemoryTransport, MultipleClientsAllReachTheServer) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto first = make_in_memory_client(bus);
    const auto second = make_in_memory_client(bus);

    std::vector<HostId> seen;
    server->on_announce([&](const ClientAnnounce& message) { seen.push_back(message.host_id); });

    first->publish_announce(announce("PC-001"));
    second->publish_announce(announce("PC-002"));

    EXPECT_EQ(seen, (std::vector<HostId>{"PC-001", "PC-002"}));
}

TEST(InMemoryTransport, EveryClientReceivesTheSameBundle) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto first = make_in_memory_client(bus);
    const auto second = make_in_memory_client(bus);

    int deliveries = 0;
    first->on_bundle([&](const TemplateBundleMessage&) { ++deliveries; });
    second->on_bundle([&](const TemplateBundleMessage&) { ++deliveries; });

    server->publish_bundle(bundle_message(1));
    EXPECT_EQ(deliveries, 2);
}

TEST(InMemoryTransport, ReportsConnectedImmediately) {
    MessageBus bus;
    const auto client = make_in_memory_client(bus);
    EXPECT_EQ(client->state(), ConnectionState::Connected);
}

TEST(InMemoryTransport, PublishingWithNoSubscriberIsHarmless) {
    MessageBus bus;
    const auto client = make_in_memory_client(bus);
    EXPECT_NO_THROW(client->publish_announce(announce("PC-001")));
}

TEST(InMemoryTransport, SubscribingFromInsideAHandlerIsSafe) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::vector<HostId> first_seen;
    std::vector<HostId> second_seen;

    // The first handler subscribes a second handler to the same topic while
    // delivery is in progress. This must not invalidate the handler list
    // deliver() is currently iterating.
    server->on_announce([&](const ClientAnnounce& message) {
        first_seen.push_back(message.host_id);
        server->on_announce(
            [&](const ClientAnnounce& inner) { second_seen.push_back(inner.host_id); });
    });

    client->publish_announce(announce("PC-001"));
    client->publish_announce(announce("PC-002"));

    EXPECT_EQ(first_seen, (std::vector<HostId>{"PC-001", "PC-002"}));
    // The handler registered during the first delivery only observes messages
    // published after it subscribed.
    EXPECT_EQ(second_seen, (std::vector<HostId>{"PC-002"}));
}

TEST(InMemoryTransport, DeliversAScriptCommandFromServerToClient) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::vector<ScriptCommand> seen;
    client->on_script_command([&](const ScriptCommand& command) { seen.push_back(command); });

    ScriptCommand command;
    command.host_id = "PC-001";
    command.run_id = "run-1";
    command.script_body = "exit 0";
    server->publish_script_command(command);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front().run_id, "run-1");
}

TEST(InMemoryTransport, DeliversAScriptResultFromClientToServer) {
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    std::vector<ScriptResultMessage> seen;
    server->on_script_result([&](const ScriptResultMessage& result) { seen.push_back(result); });

    ScriptResultMessage result;
    result.host_id = "PC-001";
    result.run_id = "run-1";
    result.status = lm::core::ScriptStatus::Completed;
    client->publish_script_result(result);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen.front().status, lm::core::ScriptStatus::Completed);
}

TEST(InMemoryTransport, DoesNotReplayACommandToALateSubscriber) {
    // The in-memory bus must model the topic's Volatile durability, or every
    // test built on it would prove the opposite of what production does.
    MessageBus bus;
    const auto server = make_in_memory_server(bus);
    const auto client = make_in_memory_client(bus);

    ScriptCommand command;
    command.host_id = "PC-001";
    command.run_id = "run-1";
    server->publish_script_command(command);

    std::vector<ScriptCommand> seen;
    client->on_script_command([&](const ScriptCommand& c) { seen.push_back(c); });

    EXPECT_TRUE(seen.empty()) << "a command must not be replayed to a client that joined later";
}
