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

TEST(FleetModel, ColoursTheCpuCellByItsLoadOncePastTheThreshold) {
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

TEST(FleetModel, LeavesAPercentageOnTheRowColourUntilItNearsItsLimit) {
    // The colour is an exception marker, not a gauge. A busy-but-fine machine
    // reads as one colour, so the one cell that does deviate is the one worth
    // looking at rather than one of three competing for attention.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    lm::transport::ResourceSampleMessage message = sample_for(QStringLiteral("PC-001"), 88.0);
    message.sample.mem_total_bytes = 1000;
    message.sample.mem_used_bytes = 800;                          // 80%
    message.sample.disks.push_back(DiskUsage{"C:\\", 1000, 250});  // 75% used
    model.apply_sample(message);

    const QColor health = FleetModel::colour_for(model.health_of(0));
    for (const int column :
         {FleetModel::CpuColumn, FleetModel::MemoryColumn, FleetModel::DiskColumn}) {
        EXPECT_EQ(foreground_at(model, 0, column), health) << "column " << column;
    }
}

TEST(FleetModel, ColoursMemoryAndDiskByTheirOwnReadingsToo) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    lm::transport::ResourceSampleMessage message = sample_for(QStringLiteral("PC-001"), 5.0);
    message.sample.mem_total_bytes = 1000;
    message.sample.mem_used_bytes = 940;  // 94%
    // {mount, total, free}: 75% used, then 97% used.
    message.sample.disks.push_back(DiskUsage{"C:\\", 1000, 250});
    message.sample.disks.push_back(DiskUsage{"D:\\", 1000, 30});
    model.apply_sample(message);

    EXPECT_EQ(foreground_at(model, 0, FleetModel::MemoryColumn), Theme::color_for_load(94.0));
    // The fullest volume, not the first or an average: a machine with one full
    // disk is in trouble however much room the others have.
    EXPECT_EQ(foreground_at(model, 0, FleetModel::DiskColumn), Theme::color_for_load(97.0));
}

TEST(FleetModel, ColoursEachPercentageColumnIndependently) {
    // An idle machine that is out of disk has to leave the CPU cell alone and
    // light the disk one, or the whole point of the colour is lost.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    lm::transport::ResourceSampleMessage message = sample_for(QStringLiteral("PC-001"), 2.0);
    message.sample.mem_total_bytes = 1000;
    message.sample.mem_used_bytes = 500;
    message.sample.disks.push_back(DiskUsage{"C:\\", 1000, 20});  // 98% full
    model.apply_sample(message);

    const QColor health = FleetModel::colour_for(model.health_of(0));
    const QColor disk = foreground_at(model, 0, FleetModel::DiskColumn);
    EXPECT_EQ(foreground_at(model, 0, FleetModel::CpuColumn), health) << "an idle CPU says nothing";
    EXPECT_NE(disk, health);
    EXPECT_GT(disk.redF() - disk.greenF(), 0.0) << "a full disk reads hot";
}

TEST(FleetModel, TheThresholdIsExclusiveSoNinetyIsStillQuiet) {
    // "Exceeds 90%" -- 90 itself is not past it. Pinned because an off-by-one
    // here is invisible on screen and only shows up as a cell that lights a
    // percentage too early, or one that never lights at all.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));
    model.apply_sample(sample_for(QStringLiteral("PC-001"), 90.0));
    EXPECT_EQ(foreground_at(model, 0, FleetModel::CpuColumn),
              FleetModel::colour_for(model.health_of(0)));

    model.apply_sample(sample_for(QStringLiteral("PC-001"), 90.5));
    EXPECT_EQ(foreground_at(model, 0, FleetModel::CpuColumn), Theme::color_for_load(90.5));
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
    // Both readings are past the threshold, so this is about the ramp itself
    // rather than about crossing into it -- a machine at 99% has to read
    // warmer than one at 92%.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    model.apply_sample(sample_for(QStringLiteral("PC-001"), 92.0));
    const QColor warm = foreground_at(model, 0, FleetModel::CpuColumn);
    model.apply_sample(sample_for(QStringLiteral("PC-001"), 99.0));
    const QColor pegged = foreground_at(model, 0, FleetModel::CpuColumn);

    EXPECT_GT(pegged.redF() - pegged.greenF(), warm.redF() - warm.greenF())
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
    model.apply_compliance(report, {});

    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Compliant);
}

TEST(FleetModelHealth, AnOnlineHostWithAFailingRuleIsFailing) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "running", ""},
                      CheckResult{"r2", CheckStatus::Fail, "not running", ""}};
    model.apply_compliance(report, {});

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
    model.apply_compliance(report, {});

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
    model.apply_compliance(report, {});
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
    EXPECT_NO_THROW(model.apply_compliance(report, {}));

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

namespace {

/// The compliance cell's text for row 0.
QString compliance_text(const FleetModel& model) {
    return model.data(model.index(0, FleetModel::ComplianceColumn), Qt::DisplayRole).toString();
}

QVector<ComplianceTag> compliance_tags(const FleetModel& model) {
    return model.data(model.index(0, FleetModel::ComplianceColumn), FleetModel::ComplianceTagsRole)
        .value<QVector<ComplianceTag>>();
}

}  // namespace

TEST(FleetModelCompliance, ScoresPassedOverChecked) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "", ""},
                      CheckResult{"r2", CheckStatus::Pass, "", ""},
                      CheckResult{"r3", CheckStatus::Fail, "", ""}};
    model.apply_compliance(report, {});

    EXPECT_EQ(compliance_text(model).toStdString(), "2 / 3");
}

TEST(FleetModelCompliance, LeavesNotApplicableOutOfTheDenominator) {
    // A rule the client cannot evaluate can never pass, so counting it would
    // park a Linux box at "2 / 3" forever with no action able to improve it.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "", ""},
                      CheckResult{"r2", CheckStatus::Pass, "", ""},
                      CheckResult{"r3", CheckStatus::NotApplicable, "", ""}};
    model.apply_compliance(report, {});

    EXPECT_EQ(compliance_text(model).toStdString(), "2 / 2");
}

TEST(FleetModelCompliance, KeepsErrorsInTheDenominator) {
    // "Could not check it" is not "passed", and must not be scored as one --
    // even though is_compliant() does not count it as a failure either.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "", ""},
                      CheckResult{"r2", CheckStatus::Error, "denied", ""}};
    model.apply_compliance(report, {});

    EXPECT_EQ(compliance_text(model).toStdString(), "1 / 2");
}

TEST(FleetModelCompliance, CarriesTheTagsItWasGiven) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Fail, "", ""},
                      CheckResult{"r2", CheckStatus::Error, "", ""}};
    model.apply_compliance(report, {ComplianceTag{QStringLiteral("Antivirus must be running"),
                                                  CheckStatus::Fail},
                                    ComplianceTag{QStringLiteral("Wire is plugged in"),
                                                  CheckStatus::Error}});

    const QVector<ComplianceTag> tags = compliance_tags(model);
    ASSERT_EQ(tags.size(), 2);
    EXPECT_EQ(tags[0].label.toStdString(), "Antivirus must be running");
    EXPECT_EQ(tags[0].status, CheckStatus::Fail);
    EXPECT_EQ(tags[1].status, CheckStatus::Error);
}

TEST(FleetModelCompliance, ASilentHostSaysSoRatherThanScoringZeroOverZero) {
    // "0 / 0" on a machine nobody has heard from reads as a clean bill of
    // health. It is the absence of one.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Missing}}));

    EXPECT_EQ(compliance_text(model).toStdString(), "Not reporting");
}

TEST(FleetModelCompliance, ASilentHostWithACachedReportLabelsItAsHistorical) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "", ""},
                      CheckResult{"r2", CheckStatus::Fail, "", ""}};
    model.apply_compliance(report, {ComplianceTag{QStringLiteral("Antivirus"), CheckStatus::Fail}});
    ASSERT_EQ(compliance_text(model).toStdString(), "1 / 2");

    model.apply(view_with({{"PC-001", HostState::Offline}}));
    EXPECT_EQ(compliance_text(model).toStdString(), "last known 1 / 2");
    // And no tags: those rules describe a machine that is no longer answering,
    // so live-looking red tags on it would claim a freshness it does not have.
    EXPECT_TRUE(compliance_tags(model).isEmpty());
}

TEST(FleetModelCompliance, AnOnlineHostWithNoReportShowsADash) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    EXPECT_EQ(compliance_text(model).toStdString(), "-");
}

TEST(FleetModelCompliance, SortsFailingHostsWorstFirst) {
    // The order the Compliance tab used to provide, now the fleet table's.
    FleetModel model;
    model.apply(view_with({{"PC-BAD", HostState::Online},
                           {"PC-CLEAN", HostState::Online},
                           {"PC-MILD", HostState::Online}}));

    auto score = [&model](const QString& host) {
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row, 0), FleetModel::HostIdRole).toString() == host) {
                return model.data(model.index(row, 0), FleetModel::SeverityRole).toInt();
            }
        }
        return -1;
    };
    auto report_for = [](const std::string& host, int passed, int failing) {
        ComplianceReport report;
        report.host_id = host;
        for (int i = 0; i < passed; ++i) {
            report.results.push_back(CheckResult{"p", CheckStatus::Pass, "", ""});
        }
        for (int i = 0; i < failing; ++i) {
            report.results.push_back(CheckResult{"f", CheckStatus::Fail, "", ""});
        }
        return report;
    };

    model.apply_compliance(report_for("PC-BAD", 0, 4), {});
    model.apply_compliance(report_for("PC-MILD", 3, 1), {});
    model.apply_compliance(report_for("PC-CLEAN", 4, 0), {});

    EXPECT_LT(score(QStringLiteral("PC-BAD")), score(QStringLiteral("PC-MILD")));
    EXPECT_LT(score(QStringLiteral("PC-MILD")), score(QStringLiteral("PC-CLEAN")));
}

TEST(FleetModelCompliance, ASilentHostOutranksEveryFailingOne) {
    // A machine nobody can hear from is an unknown; a broken rule is a known
    // problem with a known fix.
    FleetModel model;
    model.apply(view_with({{"PC-GONE", HostState::Offline}, {"PC-BAD", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-BAD";
    report.results = {CheckResult{"f", CheckStatus::Fail, "", ""}};
    model.apply_compliance(report, {});

    auto score = [&model](const QString& host) {
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row, 0), FleetModel::HostIdRole).toString() == host) {
                return model.data(model.index(row, 0), FleetModel::SeverityRole).toInt();
            }
        }
        return -1;
    };
    EXPECT_LT(score(QStringLiteral("PC-GONE")), score(QStringLiteral("PC-BAD")));
}

TEST(FleetModelCompliance, APausedHostSaysPausedRatherThanNotReporting) {
    // "Paused" is something someone did and can undo; "Not reporting" is a
    // machine to go and look at. Reading them the same way wastes a trip.
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Paused}}));

    EXPECT_EQ(compliance_text(model).toStdString(), "Paused");
}

TEST(FleetModelCompliance, APausedHostShowsItsLastKnownScoreAndNoTags) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "", ""},
                      CheckResult{"r2", CheckStatus::Fail, "", ""}};
    model.apply_compliance(report, {ComplianceTag{QStringLiteral("Antivirus"), CheckStatus::Fail}});
    ASSERT_EQ(compliance_text(model).toStdString(), "1 / 2");

    model.apply(view_with({{"PC-001", HostState::Paused}}));
    // Labelled as historical: no new report is coming while it is paused, which
    // makes the cached one exactly as stale as a dead machine's.
    EXPECT_EQ(compliance_text(model).toStdString(), "last known 1 / 2");
    EXPECT_TRUE(compliance_tags(model).isEmpty());
}

TEST(FleetModelHealth, APausedHostIsNeitherCompliantNorFailing) {
    FleetModel model;
    model.apply(view_with({{"PC-001", HostState::Online}}));

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Fail, "", ""}};
    model.apply_compliance(report, {});
    ASSERT_EQ(model.health_of(0), FleetModel::RowHealth::Failing);

    // Liveness still wins over compliance, exactly as it does for Offline.
    model.apply(view_with({{"PC-001", HostState::Paused}}));
    EXPECT_EQ(model.health_of(0), FleetModel::RowHealth::Paused);
    EXPECT_EQ(FleetModel::colour_for(FleetModel::RowHealth::Paused), QColor(Theme::kPaused));
}

TEST(FleetModelCompliance, APausedHostSortsAboveEveryOnlineHostIncludingFailingOnes) {
    FleetModel model;
    model.apply(view_with({{"PC-bad", HostState::Online},
                           {"PC-paused", HostState::Paused},
                           {"PC-good", HostState::Online}}));

    auto score = [&model](const QString& host) {
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row, 0), FleetModel::HostIdRole).toString() == host) {
                return model.data(model.index(row, 0), FleetModel::SeverityRole).toInt();
            }
        }
        return -1;
    };
    auto report_for = [](const std::string& host, int passed, int failing) {
        ComplianceReport report;
        report.host_id = host;
        for (int i = 0; i < passed; ++i) {
            report.results.push_back(CheckResult{"p", CheckStatus::Pass, "", ""});
        }
        for (int i = 0; i < failing; ++i) {
            report.results.push_back(CheckResult{"f", CheckStatus::Fail, "", ""});
        }
        return report;
    };
    model.apply_compliance(report_for("PC-bad", 0, 3), {});
    model.apply_compliance(report_for("PC-good", 3, 0), {});

    // State decides the band before compliance breaks any ties inside it, so a
    // paused machine outranks even a host failing every rule it has. That is
    // the same call Offline already makes over Failing -- a machine that is not
    // being checked is a bigger gap than a machine that is being checked and
    // found wanting, because the second one at least has a number attached.
    EXPECT_LT(score(QStringLiteral("PC-paused")), score(QStringLiteral("PC-bad")));
    EXPECT_LT(score(QStringLiteral("PC-bad")), score(QStringLiteral("PC-good")));
}
