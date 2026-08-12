#include <gtest/gtest.h>

#include <algorithm>
#include <utility>

#include "lm/core/fleet.hpp"

using namespace lm::core;
using namespace std::chrono_literals;

namespace {

const TimePoint kNow = Clock::time_point{} + 1'000'000s;

ReconcileOptions options(std::uint64_t revision = 0) {
    ReconcileOptions opts;
    opts.liveliness_lease = 10s;
    opts.current_revision = revision;
    return opts;
}

DiscoveredClient client(HostId id, TimePoint last_seen, std::uint64_t revision = 0) {
    DiscoveredClient c;
    c.host_id = std::move(id);
    c.last_seen = last_seen;
    c.applied_revision = revision;
    c.caps = platform_capabilities();
    return c;
}

const FleetEntry& entry_for(const FleetView& view, const HostId& id) {
    const auto found = std::find_if(view.entries.begin(), view.entries.end(),
                                    [&](const FleetEntry& e) { return e.host_id == id; });
    EXPECT_NE(found, view.entries.end());
    return *found;
}

}  // namespace

// --- the four states -------------------------------------------------------

TEST(Reconcile, ExpectedAndReportingWithinLeaseIsOnline) {
    const auto view = reconcile({{"PC-001", "10.0.0.1"}}, {client("PC-001", kNow - 2s)}, kNow,
                                options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Online);
    EXPECT_EQ(view.counts.online, 1u);
}

TEST(Reconcile, ExpectedButSilentBeyondLeaseIsOffline) {
    const auto view = reconcile({{"PC-001", "10.0.0.1"}}, {client("PC-001", kNow - 30s)}, kNow,
                                options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Offline);
    EXPECT_EQ(view.counts.offline, 1u);
}

TEST(Reconcile, ExpectedButNeverSeenIsMissing) {
    const auto view = reconcile({{"PC-001", "10.0.0.1"}}, {}, kNow, options());
    const FleetEntry& entry = entry_for(view, "PC-001");
    EXPECT_EQ(entry.state, HostState::Missing);
    EXPECT_FALSE(entry.last_seen.has_value());
    EXPECT_EQ(view.counts.missing, 1u);
}

TEST(Reconcile, ReportingButNotExpectedIsUnexpected) {
    const auto view = reconcile({}, {client("ROGUE-PC", kNow - 1s)}, kNow, options());
    EXPECT_EQ(entry_for(view, "ROGUE-PC").state, HostState::Unexpected);
    EXPECT_EQ(view.counts.unexpected, 1u);
}

TEST(Reconcile, UnexpectedStaysUnexpectedBeyondTheLease) {
    const auto view = reconcile({}, {client("ROGUE-PC", kNow - 500s)}, kNow, options());
    EXPECT_EQ(entry_for(view, "ROGUE-PC").state, HostState::Unexpected);
}

// --- lease boundary --------------------------------------------------------

TEST(Reconcile, ExactlyAtTheLeaseBoundaryIsStillOnline) {
    const auto view = reconcile({{"PC-001", ""}}, {client("PC-001", kNow - 10s)}, kNow, options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Online);
}

TEST(Reconcile, OneTickPastTheLeaseIsOffline) {
    const auto view =
        reconcile({{"PC-001", ""}}, {client("PC-001", kNow - 10s - 1ms)}, kNow, options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Offline);
}

TEST(Reconcile, ClockSkewFromTheFutureIsTreatedAsOnline) {
    const auto view = reconcile({{"PC-001", ""}}, {client("PC-001", kNow + 5s)}, kNow, options());
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Online);
}

// --- staleness -------------------------------------------------------------

TEST(Reconcile, ClientOnAnOlderRevisionIsStale) {
    const auto view =
        reconcile({{"PC-001", ""}}, {client("PC-001", kNow, 3)}, kNow, options(5));
    EXPECT_TRUE(entry_for(view, "PC-001").stale);
    EXPECT_EQ(view.counts.stale, 1u);
}

TEST(Reconcile, ClientOnTheCurrentRevisionIsNotStale) {
    const auto view =
        reconcile({{"PC-001", ""}}, {client("PC-001", kNow, 5)}, kNow, options(5));
    EXPECT_FALSE(entry_for(view, "PC-001").stale);
    EXPECT_EQ(view.counts.stale, 0u);
}

TEST(Reconcile, MissingHostsAreNeverStale) {
    const auto view = reconcile({{"PC-001", ""}}, {}, kNow, options(5));
    EXPECT_FALSE(entry_for(view, "PC-001").stale);
    EXPECT_EQ(view.counts.stale, 0u);
}

TEST(Reconcile, OfflineHostsStillReportTheirLastAppliedRevision) {
    const auto view =
        reconcile({{"PC-001", ""}}, {client("PC-001", kNow - 60s, 3)}, kNow, options(5));
    const FleetEntry& entry = entry_for(view, "PC-001");
    EXPECT_EQ(entry.state, HostState::Offline);
    EXPECT_TRUE(entry.stale);
}

// --- metadata and ordering -------------------------------------------------

TEST(Reconcile, CarriesTheConfiguredAddress) {
    const auto view = reconcile({{"PC-001", "10.0.0.7"}}, {client("PC-001", kNow)}, kNow, options());
    EXPECT_EQ(entry_for(view, "PC-001").address, "10.0.0.7");
}

TEST(Reconcile, UnexpectedHostsHaveNoConfiguredAddress) {
    const auto view = reconcile({}, {client("ROGUE-PC", kNow)}, kNow, options());
    EXPECT_TRUE(entry_for(view, "ROGUE-PC").address.empty());
}

TEST(Reconcile, SortsMostUrgentFirstThenByName) {
    const std::vector<ExpectedHost> expected{
        {"PC-ONLINE-B", ""}, {"PC-ONLINE-A", ""}, {"PC-MISSING", ""}, {"PC-OFFLINE", ""}};
    const std::vector<DiscoveredClient> discovered{client("PC-ONLINE-B", kNow),
                                                   client("PC-ONLINE-A", kNow),
                                                   client("PC-OFFLINE", kNow - 60s),
                                                   client("ROGUE", kNow)};

    const auto view = reconcile(expected, discovered, kNow, options());

    std::vector<HostId> order;
    for (const FleetEntry& entry : view.entries) {
        order.push_back(entry.host_id);
    }
    EXPECT_EQ(order, (std::vector<HostId>{"PC-MISSING", "PC-OFFLINE", "ROGUE", "PC-ONLINE-A",
                                          "PC-ONLINE-B"}));
}

TEST(Reconcile, EmptyInputsProduceAnEmptyView) {
    const auto view = reconcile({}, {}, kNow, options());
    EXPECT_TRUE(view.entries.empty());
    EXPECT_EQ(view.counts.online, 0u);
    EXPECT_EQ(view.counts.missing, 0u);
}

TEST(Reconcile, DuplicateDiscoveryReportsCollapseToOneEntry) {
    const auto view = reconcile({{"PC-001", ""}},
                                {client("PC-001", kNow - 60s), client("PC-001", kNow)}, kNow,
                                options());
    EXPECT_EQ(view.entries.size(), 1u);
    // The most recent sighting wins.
    EXPECT_EQ(entry_for(view, "PC-001").state, HostState::Online);
}
