#include <gtest/gtest.h>

#include <QHeaderView>
#include <QImage>
#include <QTableView>

#include "lm/ui/compliance_tag_delegate.hpp"
#include "lm/ui/fleet_model.hpp"
#include "lm/ui/theme.hpp"
#include "pixel_probe.hpp"

using namespace lm::core;
using namespace lm::ui;

namespace {

/// A one-row table showing only the compliance column, with the delegate under
/// test installed on it. Sized rather than shown: these assertions are about
/// what the delegate draws, and render() paints from scratch regardless.
class Fixture {
public:
    explicit Fixture(int width = 420) {
        model.apply(view());
        table.setModel(&model);
        table.setItemDelegateForColumn(FleetModel::ComplianceColumn,
                                       new ComplianceTagDelegate(&table));
        table.verticalHeader()->hide();
        for (int column = 0; column < FleetModel::ColumnCount; ++column) {
            table.setColumnHidden(column, column != FleetModel::ComplianceColumn);
        }
        table.setColumnWidth(FleetModel::ComplianceColumn, width);
        table.resize(width + 4, 80);
    }

    void report(std::vector<CheckStatus> statuses, QVector<ComplianceTag> tags) {
        ComplianceReport report;
        report.host_id = "PC-001";
        int n = 0;
        for (const CheckStatus status : statuses) {
            report.results.push_back(CheckResult{"r" + std::to_string(n++), status, "", ""});
        }
        model.apply_compliance(report, std::move(tags));
    }

    QImage paint() { return lm::ui::test::paint(table); }

    FleetModel model;
    QTableView table;

private:
    static FleetView view() {
        FleetEntry entry;
        entry.host_id = "PC-001";
        entry.state = HostState::Online;
        FleetView fleet;
        fleet.entries = {entry};
        return fleet;
    }
};

}  // namespace

TEST(ComplianceTagDelegate, PaintsAFailingTagInTheFailColour) {
    Fixture fixture;
    fixture.report({CheckStatus::Fail},
                   {ComplianceTag{QStringLiteral("Antivirus"), CheckStatus::Fail}});

    // Theme::color_for(Fail), not a colour picked in the delegate: the detail
    // pane paints the same statuses and the two must not disagree.
    EXPECT_TRUE(lm::ui::test::contains_colour(fixture.paint(), Theme::color_for(CheckStatus::Fail)));
}

TEST(ComplianceTagDelegate, PaintsAnErrorTagInADifferentColourFromAFailure) {
    // A rule that is broken and a rule that could not be read are different
    // problems; a tag that renders them identically says otherwise.
    Fixture fixture;
    fixture.report({CheckStatus::Error},
                   {ComplianceTag{QStringLiteral("Registry read"), CheckStatus::Error}});

    const QImage image = fixture.paint();
    EXPECT_TRUE(lm::ui::test::contains_colour(image, Theme::color_for(CheckStatus::Error)));
    EXPECT_NE(Theme::color_for(CheckStatus::Error), Theme::color_for(CheckStatus::Fail));
}

TEST(ComplianceTagDelegate, PaintsNoTagColourWhenNothingIsFailing) {
    Fixture fixture;
    fixture.report({CheckStatus::Pass}, {});

    EXPECT_FALSE(
        lm::ui::test::contains_colour(fixture.paint(), Theme::color_for(CheckStatus::Fail)));
}

TEST(ComplianceTagDelegate, DrawsTheRatioEvenWithNoRoomForTags) {
    // The count is the one thing that must survive any width: it is what says
    // whether the host needs looking at at all.
    Fixture fixture(60);
    fixture.report({CheckStatus::Pass, CheckStatus::Fail},
                   {ComplianceTag{QStringLiteral("A very long rule description indeed"),
                                  CheckStatus::Fail}});

    // The row's health colour, which is what the ratio text is drawn in.
    EXPECT_TRUE(lm::ui::test::contains_colour(
        fixture.paint(), FleetModel::colour_for(FleetModel::RowHealth::Failing)));
}

TEST(ComplianceTagDelegate, SizeHintDoesNotGrowWithoutBoundAsTagsAreAdded) {
    // resizeColumnsToContents() would otherwise ask for a column wider than the
    // window for a host failing twenty rules -- which is the situation the
    // "+N more" marker exists to handle.
    Fixture few;
    few.report({CheckStatus::Fail}, {ComplianceTag{QStringLiteral("One"), CheckStatus::Fail}});

    QVector<ComplianceTag> many;
    std::vector<CheckStatus> statuses;
    for (int i = 0; i < 20; ++i) {
        many.push_back(ComplianceTag{QStringLiteral("Rule number %1").arg(i), CheckStatus::Fail});
        statuses.push_back(CheckStatus::Fail);
    }
    Fixture lots;
    lots.report(statuses, many);

    const QModelIndex few_index = few.model.index(0, FleetModel::ComplianceColumn);
    const QModelIndex lots_index = lots.model.index(0, FleetModel::ComplianceColumn);
    QStyleOptionViewItem option;
    ComplianceTagDelegate delegate;
    const int few_width = delegate.sizeHint(option, few_index).width();
    const int lots_width = delegate.sizeHint(option, lots_index).width();

    EXPECT_LT(lots_width, few_width * 6)
        << "few=" << few_width << " lots=" << lots_width;
}
