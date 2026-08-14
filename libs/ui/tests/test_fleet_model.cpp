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
