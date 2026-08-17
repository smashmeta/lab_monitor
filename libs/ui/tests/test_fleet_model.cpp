#include <gtest/gtest.h>

#include <QSignalSpy>

#include "lm/ui/fleet_model.hpp"
#include "lm/ui/theme.hpp"

using namespace lm::core;
using namespace lm::ui;
using namespace std::chrono_literals;

namespace {

const TimePoint kNow = Clock::time_point{} + 1'000'000s;

FleetView view_with(std::vector<std::pair<HostId, HostState>> hosts) {
    FleetView view;
    for (auto& [id, state] : hosts) {
        FleetEntry entry;
        entry.host_id = id;
        entry.state = state;
        entry.last_seen = kNow;
        view.entries.push_back(entry);
    }
    return view;
}

int row_of(const FleetModel& model, const QString& host_id) {
    for (int row = 0; row < model.rowCount(); ++row) {
        if (model.data(model.index(row, 0), FleetModel::HostIdRole).toString() == host_id) {
            return row;
        }
    }
    return -1;
}

}  // namespace

namespace {

/// A sample for one host, as the transport delivers it.
lm::transport::ResourceSampleMessage sample_for(const QString& host_id, double cpu_percent) {
    lm::transport::ResourceSampleMessage message;
    message.host_id = host_id.toStdString();
    message.sample.cpu_percent = cpu_percent;
    message.sample.mem_total_bytes = 16'000'000'000;
    message.sample.mem_used_bytes = 8'000'000'000;
    return message;
}

QColor foreground_at(const FleetModel& model, int row, int column) {
    return model.data(model.index(row, column), Qt::ForegroundRole).value<QColor>();
}

}  // namespace

TEST(FleetModel, ColoursTheCpuCellByItsLoadRatherThanTheRowHealth) {
    // A machine can be perfectly compliant and pegged at 100%. Painting the
    // CPU cell with the row's health colour hides exactly that.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));
    model.apply_sample(sample_for(QStringLiteral("PC-001"), 96.0));

    EXPECT_EQ(foreground_at(model, 0, FleetModel::CpuColumn), Theme::color_for_load(96.0));
    EXPECT_NE(foreground_at(model, 0, FleetModel::CpuColumn),
              foreground_at(model, 0, FleetModel::HostColumn))
        << "the CPU cell must not still be taking the row's colour";
}

TEST(FleetModel, ColoursMemoryAndDiskByTheirOwnReadingsToo) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    lm::transport::ResourceSampleMessage message = sample_for(QStringLiteral("PC-001"), 5.0);
    message.sample.mem_total_bytes = 1000;
    message.sample.mem_used_bytes = 800;  // 80%
    // {mount, total, free}: 75% used, then 97% used.
    message.sample.disks.push_back(DiskUsage{"C:\\", 1000, 250});
    message.sample.disks.push_back(DiskUsage{"D:\\", 1000, 30});
    model.apply_sample(message);

    EXPECT_EQ(foreground_at(model, 0, FleetModel::MemoryColumn), Theme::color_for_load(80.0));
    // The fullest volume, not the first or an average: a machine with one full
    // disk is in trouble however much room the others have.
    EXPECT_EQ(foreground_at(model, 0, FleetModel::DiskColumn), Theme::color_for_load(97.0));
}

TEST(FleetModel, ColoursEachPercentageColumnIndependently) {
    // An idle machine that is out of disk has to show green CPU and red disk in
    // the same row, or the whole point of the colour is lost.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    lm::transport::ResourceSampleMessage message = sample_for(QStringLiteral("PC-001"), 2.0);
    message.sample.mem_total_bytes = 1000;
    message.sample.mem_used_bytes = 500;
    message.sample.disks.push_back(DiskUsage{"C:\\", 1000, 20});  // 98% full
    model.apply_sample(message);

    const QColor cpu = foreground_at(model, 0, FleetModel::CpuColumn);
    const QColor disk = foreground_at(model, 0, FleetModel::DiskColumn);
    EXPECT_LT(cpu.redF() - cpu.greenF(), 0.0) << "an idle CPU reads cool";
    EXPECT_GT(disk.redF() - disk.greenF(), 0.0) << "a full disk reads hot";
}

TEST(FleetModel, LeavesTheNonPercentageColumnsOnTheRowHealthColour) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));
    model.apply_sample(sample_for(QStringLiteral("PC-001"), 96.0));

    const QColor health = FleetModel::colour_for(model.health_of(0));
    for (const int column : {FleetModel::HostColumn, FleetModel::StateColumn,
                             FleetModel::RevisionColumn, FleetModel::LastSeenColumn}) {
        EXPECT_EQ(foreground_at(model, 0, column), health) << "column " << column;
    }
}

TEST(FleetModel, KeepsTheHealthColourOnAPercentageColumnWithNothingToReport) {
    // No disks in the sample at all: the cell shows "-", so there is no reading
    // for it to be coloured by.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));
    model.apply_sample(sample_for(QStringLiteral("PC-001"), 96.0));

    EXPECT_EQ(model.data(model.index(0, FleetModel::DiskColumn), Qt::DisplayRole).toString(),
              QStringLiteral("-"));
    EXPECT_EQ(foreground_at(model, 0, FleetModel::DiskColumn),
              FleetModel::colour_for(model.health_of(0)));
}

TEST(FleetModel, TracksTheLoadColourAsTheReadingChanges) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    model.apply_sample(sample_for(QStringLiteral("PC-001"), 3.0));
    const QColor idle = foreground_at(model, 0, FleetModel::CpuColumn);
    model.apply_sample(sample_for(QStringLiteral("PC-001"), 99.0));
    const QColor pegged = foreground_at(model, 0, FleetModel::CpuColumn);

    EXPECT_GT(pegged.redF() - pegged.greenF(), idle.redF() - idle.greenF())
        << "a busier machine has to read warmer";
}

TEST(FleetModel, AnnouncesEveryColumnWhenTheStateChanges) {
    // The health colour paints the whole row, and the CPU cell's colour depends
    // on the state as well -- so announcing only the first two columns leaves
    // the rest of the row showing the colour it had before the change.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));
    QSignalSpy spy(&model, &QAbstractItemModel::dataChanged);
    ASSERT_TRUE(spy.isValid());

    model.apply(view_with({{"PC-001", HostState::Offline}}));

    ASSERT_GT(spy.count(), 0) << "a state change has to be announced at all";
    int rightmost = -1;
    for (const QList<QVariant>& emission : spy) {
        rightmost = std::max(rightmost, emission.at(1).value<QModelIndex>().column());
    }
    EXPECT_EQ(rightmost, FleetModel::ColumnCount - 1)
        << "the announcement stops at column " << rightmost;
}

TEST(FleetModel, KeepsTheHealthColourOnTheCpuCellBeforeAnySampleArrives) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    EXPECT_EQ(foreground_at(model, 0, FleetModel::CpuColumn),
              FleetModel::colour_for(model.health_of(0)))
        << "an empty cell has no load to report";
}

TEST(FleetModel, KeepsTheHealthColourOnTheCpuCellOnceTheHostGoesQuiet) {
    // The last sample is stale the moment the host stops reporting. Painted
    // green it would look like a live reading of an idle machine.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));
    model.apply_sample(sample_for(QStringLiteral("PC-001"), 4.0));
    model.apply(view_with({{"PC-001", HostState::Offline}}));

    EXPECT_EQ(foreground_at(model, 0, FleetModel::CpuColumn),
              FleetModel::colour_for(model.health_of(0)));
}

namespace {

FleetView view_with_network_capable_host() {
    FleetView view;
    FleetEntry entry;
    entry.host_id = "PC-001";
    entry.state = HostState::Online;
    entry.last_seen = kNow;
    entry.caps = Capabilities{}.add(Capability::Resources).add(Capability::Network);
    view.entries.push_back(entry);
    return view;
}

QString display_at(const FleetModel& model, int row, int column) {
    return model.data(model.index(row, column), Qt::DisplayRole).toString();
}

}  // namespace

TEST(FleetModel, CountsConnectedAdaptersAgainstTheTotal) {
    FleetModel model;
    model.apply(view_with_network_capable_host());

    lm::transport::ResourceSampleMessage message;
    message.host_id = "PC-001";
    message.sample.adapters = {
        NetworkAdapter{"eth0", "Onboard NIC", "{eth0-guid}", AdapterType::Ethernet, LinkState::Connected},
        NetworkAdapter{"wlan0", "Wireless", "{wlan0-guid}", AdapterType::WiFi, LinkState::Disconnected},
        NetworkAdapter{"Site VPN", "RAS entry Site VPN", "{Site VPN-guid}", AdapterType::Tunnel, LinkState::Disconnected},
    };
    model.apply_sample(message);

    EXPECT_EQ(display_at(model, 0, FleetModel::AdaptersColumn).toStdString(), "1 / 3")
        << "the count alone would not say whether the machine is on the network";
}

TEST(FleetModel, ShowsADashWhenTheClientCannotReportAdapters) {
    // An old client reporting nothing and a machine with no adapters are
    // different facts. "0 / 0" would state the wrong one.
    FleetModel model;
    FleetView view;
    FleetEntry entry;
    entry.host_id = "PC-001";
    entry.state = HostState::Online;
    entry.caps = Capabilities{}.add(Capability::Resources);
    view.entries.push_back(entry);
    model.apply(view);

    EXPECT_EQ(display_at(model, 0, FleetModel::AdaptersColumn).toStdString(), "-");
    EXPECT_TRUE(model.data(model.index(0, FleetModel::AdaptersColumn), Qt::ToolTipRole)
                    .toString()
                    .contains(QStringLiteral("does not report")));
}

TEST(FleetModel, ShowsZeroOfZeroForACapableClientWithNoAdapters) {
    FleetModel model;
    model.apply(view_with_network_capable_host());

    EXPECT_EQ(display_at(model, 0, FleetModel::AdaptersColumn).toStdString(), "0 / 0");
}

TEST(FleetModel, ListsEveryAdapterInTheTooltip) {
    FleetModel model;
    model.apply(view_with_network_capable_host());

    lm::transport::ResourceSampleMessage message;
    message.host_id = "PC-001";
    message.sample.adapters = {
        NetworkAdapter{"smash-lan", "Onboard NIC", "{eth0-guid}", AdapterType::Ethernet,
                        LinkState::NoMedia},
        NetworkAdapter{"Lab Dialup", "RAS entry Lab Dialup", "{dialup-guid}", AdapterType::Modem,
                        LinkState::Disconnected},
    };
    model.apply_sample(message);

    const QString tooltip =
        model.data(model.index(0, FleetModel::AdaptersColumn), Qt::ToolTipRole).toString();
    // Names, not descriptions: the tooltip is read the way the fleet list is,
    // and "smash-lan" is what the machine's own UI calls it.
    EXPECT_TRUE(tooltip.contains(QStringLiteral("smash-lan"))) << tooltip.toStdString();
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Lab Dialup")));
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Modem")));
    // The distinction the boolean could not make: an unplugged cable is not
    // the same as a dial-up entry sitting idle.
    EXPECT_TRUE(tooltip.contains(QStringLiteral("No link")));
    EXPECT_TRUE(tooltip.contains(QStringLiteral("Disconnected")));
}

TEST(FleetModel, CountsOnlyFullyConnectedAdaptersAsUp) {
    // An adapter that is enabled with nothing plugged in, one that is
    // negotiating, and one that is switched off are all "not up" for the
    // column's purposes, however different they are to look at.
    FleetModel model;
    model.apply(view_with_network_capable_host());

    lm::transport::ResourceSampleMessage message;
    message.host_id = "PC-001";
    message.sample.adapters = {
        NetworkAdapter{"a", "", "{a}", AdapterType::Ethernet, LinkState::Connected},
        NetworkAdapter{"b", "", "{b}", AdapterType::Ethernet, LinkState::NoMedia},
        NetworkAdapter{"c", "", "{c}", AdapterType::WiFi, LinkState::Connecting},
        NetworkAdapter{"d", "", "{d}", AdapterType::Ethernet, LinkState::Disabled},
        NetworkAdapter{"e", "", "{e}", AdapterType::Ethernet, LinkState::Faulted},
    };
    model.apply_sample(message);

    EXPECT_EQ(display_at(model, 0, FleetModel::AdaptersColumn).toStdString(), "1 / 5");
}

TEST(FleetModel, StartsEmpty) {
    FleetModel model;
    EXPECT_EQ(model.rowCount(), 0);
    EXPECT_GT(model.columnCount(), 0);
}

TEST(FleetModel, InsertsRowsWithoutResetting) {
    FleetModel model;
    QSignalSpy reset_spy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy insert_spy(&model, &QAbstractItemModel::rowsInserted);

    model.apply(view_with({{"PC-002", HostState::Online}, {"PC-001", HostState::Online}}));

    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_EQ(reset_spy.count(), 0);
    EXPECT_GT(insert_spy.count(), 0);
}

TEST(FleetModel, KeepsRowsInStableHostIdOrder) {
    FleetModel model;
    model.apply(view_with({{"PC-003", HostState::Online},
                           {"PC-001", HostState::Missing},
                           {"PC-002", HostState::Offline}}));

    EXPECT_EQ(row_of(model, "PC-001"), 0);
    EXPECT_EQ(row_of(model, "PC-002"), 1);
    EXPECT_EQ(row_of(model, "PC-003"), 2);
}

TEST(FleetModel, StateChangeDoesNotMoveTheRow) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}, {"PC-002", HostState::Online}}));
    const int before = row_of(model, "PC-002");

    QSignalSpy move_spy(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy reset_spy(&model, &QAbstractItemModel::modelReset);
    model.apply(view_with({{"PC-001", HostState::Online}, {"PC-002", HostState::Missing}}));

    EXPECT_EQ(row_of(model, "PC-002"), before);
    EXPECT_EQ(move_spy.count(), 0);
    EXPECT_EQ(reset_spy.count(), 0);
}

TEST(FleetModel, SeverityRoleDrivesProxySorting) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}, {"PC-002", HostState::Missing}}));

    const int online = model.data(model.index(row_of(model, "PC-001"), 0),
                                  FleetModel::SeverityRole).toInt();
    const int missing = model.data(model.index(row_of(model, "PC-002"), 0),
                                   FleetModel::SeverityRole).toInt();
    EXPECT_LT(missing, online);  // lower sorts first: Missing is most urgent
}

TEST(FleetModel, RemovesHostsThatLeaveTheView) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}, {"PC-002", HostState::Unexpected}}));

    QSignalSpy remove_spy(&model, &QAbstractItemModel::rowsRemoved);
    model.apply(view_with({{"PC-001", HostState::Online}}));

    EXPECT_EQ(model.rowCount(), 1);
    EXPECT_EQ(row_of(model, "PC-002"), -1);
    EXPECT_GT(remove_spy.count(), 0);
}

TEST(FleetModel, UnchangedViewEmitsNoDataChanged) {
    FleetModel model;
    const FleetView view = view_with({{"PC-001", HostState::Online}});
    model.apply(view);

    QSignalSpy changed_spy(&model, &QAbstractItemModel::dataChanged);
    model.apply(view);

    EXPECT_EQ(changed_spy.count(), 0);
}

TEST(FleetModel, SampleUpdatesOnlyTheResourceColumns) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    QSignalSpy changed_spy(&model, &QAbstractItemModel::dataChanged);

    lm::transport::ResourceSampleMessage sample;
    sample.host_id = "PC-001";
    sample.sample.cpu_percent = 55.0;
    sample.sample.mem_total_bytes = 1000;
    sample.sample.mem_used_bytes = 500;
    model.apply_sample(sample);

    ASSERT_GT(changed_spy.count(), 0);
    const auto arguments = changed_spy.takeFirst();
    const auto top_left = arguments.at(0).toModelIndex();
    const auto bottom_right = arguments.at(1).toModelIndex();
    EXPECT_EQ(top_left.row(), bottom_right.row());
    // A contiguous resource block, not the whole row.
    EXPECT_LT(bottom_right.column() - top_left.column() + 1, model.columnCount());
}

TEST(FleetModel, SampleForAnUnknownHostIsIgnored) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    lm::transport::ResourceSampleMessage sample;
    sample.host_id = "GHOST";
    EXPECT_NO_THROW(model.apply_sample(sample));
    EXPECT_EQ(model.rowCount(), 1);
}

TEST(FleetModelHealth, UnexpectedHostsReadAsUnexpectedRegardlessOfCompliance) {
    FleetModel model;
    model.apply(view_with({{"ROGUE", HostState::Unexpected}}));

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Unexpected);
}

TEST(FleetModelHealth, AnOfflineExpectedHostReadsAsOffline) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Offline}}));

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Offline);
}

TEST(FleetModelHealth, AMissingExpectedHostIsDistinctFromOffline) {
    // Never seen at all is a different problem from stopped reporting, and the
    // two must not paint the same.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Missing}}));

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Missing);
}

TEST(FleetModelHealth, AnOnlineHostWithNoReportYetIsUnknownNotCompliant) {
    // Claiming compliance before any report has arrived would be a green light
    // for a machine nobody has actually checked.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Unknown);
}

TEST(FleetModelHealth, AnOnlineHostWhoseRulesAllPassIsCompliant) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "running", ""}};
    model.apply_compliance(report);

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Compliant);
}

TEST(FleetModelHealth, AnOnlineHostWithAFailingRuleIsFailing) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "running", ""},
                      CheckResult{"r2", CheckStatus::Fail, "not running", ""}};
    model.apply_compliance(report);

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Failing);
}

TEST(FleetModelHealth, NotApplicableAndErrorDoNotCountAsFailures) {
    // Matches core::is_compliant: a Linux box cannot fail a registry rule, and
    // a transient probe error is not a violation.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::NotApplicable, "", ""},
                      CheckResult{"r2", CheckStatus::Error, "read failed", "denied"}};
    model.apply_compliance(report);

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Compliant);
}

TEST(FleetModelHealth, AnOfflineHostKeepsOfflineEvenWithAFailingReport) {
    // State wins: a machine that is not there cannot be usefully described by
    // the last rules it happened to fail.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Fail, "not running", ""}};
    model.apply_compliance(report);
    ASSERT_EQ(model.health_of(0), FleetModel::RowHealth::Failing);

    model.apply(view_with({{"PC-001", HostState::Offline}}));
    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Offline);
}

TEST(FleetModelHealth, EveryColumnCarriesTheHealthColour) {
    FleetModel model;
    model.apply(view_with({{"ROGUE", HostState::Unexpected}}));

    for (int column = 0; column < model.columnCount(); ++column) {
        const QVariant colour = model.data(model.index(0, column), Qt::ForegroundRole);
        ASSERT_TRUE(colour.isValid()) << "column " << column << " has no foreground";
        EXPECT_EQ(colour.value<QColor>(), QColor(lm::ui::Theme::kUnexpected));
    }
}

TEST(FleetModelHealth, ComplianceForAnUnknownHostIsIgnored) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "GHOST";
    report.results = {CheckResult{"r1", CheckStatus::Fail, "", ""}};
    EXPECT_NO_THROW(model.apply_compliance(report));

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Unknown);
}

TEST(FleetModel, ProvidesHeadersForEveryColumn) {
    FleetModel model;
    for (int column = 0; column < model.columnCount(); ++column) {
        EXPECT_FALSE(model.headerData(column, Qt::Horizontal, Qt::DisplayRole)
                         .toString()
                         .isEmpty());
    }
}
