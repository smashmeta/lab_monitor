#include <gtest/gtest.h>

#include "lm/core/client_registry.hpp"

using namespace lm::core;
using namespace std::chrono_literals;

namespace {
const TimePoint kNow = Clock::time_point{} + 1'000'000s;
}

TEST(ClientRegistry, StartsEmpty) {
    const ClientRegistry registry;
    EXPECT_TRUE(registry.snapshot().empty());
}

TEST(ClientRegistry, RecordsAnAnnouncement) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}.add(Capability::Resources), kNow);

    const auto clients = registry.snapshot();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients.front().host_id, "PC-001");
    EXPECT_TRUE(clients.front().caps.has(Capability::Resources));
    EXPECT_EQ(clients.front().last_seen, kNow);
}

TEST(ClientRegistry, SamplesRefreshLastSeenWithoutDuplicating) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}, kNow);
    registry.record_sample("PC-001", kNow + 5s);

    const auto clients = registry.snapshot();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients.front().last_seen, kNow + 5s);
}

TEST(ClientRegistry, ASampleFromAnUnannouncedHostStillRegistersIt) {
    ClientRegistry registry;
    registry.record_sample("ROGUE", kNow);
    EXPECT_EQ(registry.snapshot().size(), 1u);
}

TEST(ClientRegistry, ReportsRecordTheAppliedRevision) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}, kNow);
    registry.record_report("PC-001", 7, kNow + 1s);

    EXPECT_EQ(registry.snapshot().front().applied_revision, 7u);
}

TEST(ClientRegistry, AnnouncementDoesNotResetAKnownRevision) {
    ClientRegistry registry;
    registry.record_report("PC-001", 7, kNow);
    registry.record_announce("PC-001", Capabilities{}, kNow + 1s);

    EXPECT_EQ(registry.snapshot().front().applied_revision, 7u);
}

TEST(ClientRegistry, MarkLostRemovesTheClient) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}, kNow);
    registry.mark_lost("PC-001");
    EXPECT_TRUE(registry.snapshot().empty());
}

TEST(ClientRegistry, MarkLostForAnUnknownHostIsHarmless) {
    ClientRegistry registry;
    EXPECT_NO_THROW(registry.mark_lost("GHOST"));
}

TEST(ClientRegistry, SnapshotIsOrderedByHostId) {
    ClientRegistry registry;
    registry.record_announce("PC-003", Capabilities{}, kNow);
    registry.record_announce("PC-001", Capabilities{}, kNow);
    registry.record_announce("PC-002", Capabilities{}, kNow);

    const auto clients = registry.snapshot();
    ASSERT_EQ(clients.size(), 3u);
    EXPECT_EQ(clients[0].host_id, "PC-001");
    EXPECT_EQ(clients[2].host_id, "PC-003");
}

TEST(ClientRegistry, OutOfOrderSampleDoesNotRewindLastSeen) {
    ClientRegistry registry;
    registry.record_sample("PC-001", kNow + 10s);
    registry.record_sample("PC-001", kNow + 2s);

    const auto clients = registry.snapshot();
    ASSERT_EQ(clients.size(), 1u);
    EXPECT_EQ(clients.front().last_seen, kNow + 10s);
}

TEST(ClientRegistry, FeedsReconcileDirectly) {
    ClientRegistry registry;
    registry.record_announce("PC-001", Capabilities{}, kNow);

    ReconcileOptions options;
    options.liveliness_lease = 10s;
    const FleetView view = reconcile({{"PC-001", ""}}, registry.snapshot(), kNow, options);

    EXPECT_EQ(view.counts.online, 1u);
}
