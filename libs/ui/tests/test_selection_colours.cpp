#include <gtest/gtest.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QImage>
#include <QTableView>

#include <chrono>

#include "lm/core/fleet.hpp"
#include "lm/ui/fleet_model.hpp"
#include "lm/ui/keep_foreground_delegate.hpp"
#include "lm/ui/theme.hpp"
#include "pixel_probe.hpp"

using namespace lm::core;
using namespace lm::ui;
using namespace std::chrono_literals;

namespace {

/// A single Unexpected host, so the whole viewport is one row painted
/// kUnexpected -- a hue far enough from kText that one cannot be mistaken for
/// the other at any antialiasing blend.
FleetView one_unexpected_host() {
    FleetEntry entry;
    entry.host_id = "PC-001";
    entry.state = HostState::Unexpected;
    entry.last_seen = Clock::time_point{} + 1'000'000s;

    FleetView view;
    view.entries.push_back(entry);
    return view;
}

using lm::ui::test::contains_colour;

/// Paints the fleet table's viewport the way the server does, with row 0
/// either selected or not, and hands back the pixels.
QImage paint_fleet_table(bool select_row) {
    FleetModel model;
    model.apply(one_unexpected_host());

    QTableView view;
    view.setModel(&model);
    view.setItemDelegate(new KeepForegroundDelegate(&view));
    view.setSelectionBehavior(QAbstractItemView::SelectRows);
    view.resize(700, 120);

    // show() is what polishes the widget against the stylesheet; painting a
    // never-shown view would test the unstyled fallback instead.
    view.show();
    QApplication::processEvents();

    if (select_row) {
        view.selectRow(0);
        QApplication::processEvents();
    }

    return lm::ui::test::paint(*view.viewport());
}

}  // namespace

TEST(SelectionColours, UnselectedRowPaintsItsHealthColour) {
    EXPECT_TRUE(contains_colour(paint_fleet_table(false), QColor(Theme::kUnexpected)));
}

TEST(SelectionColours, SelectedRowKeepsItsHealthColour) {
    EXPECT_TRUE(contains_colour(paint_fleet_table(true), QColor(Theme::kUnexpected)))
        << "a selected row lost the health hue it carries when unselected";
}

TEST(SelectionColours, SelectedRowIsNotRepaintedInTheDefaultTextColour) {
    EXPECT_FALSE(contains_colour(paint_fleet_table(true), QColor(Theme::kText)))
        << "something repainted the selected row in kText, discarding its health hue";
}
