#include <gtest/gtest.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QColor>
#include <QImage>
#include <QTableView>

#include <chrono>
#include <cstdlib>

#include "lm/core/fleet.hpp"
#include "lm/ui/fleet_model.hpp"
#include "lm/ui/keep_foreground_delegate.hpp"
#include "lm/ui/theme.hpp"

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

bool contains_colour(const QImage& image, const QColor& colour, int tolerance = 8) {
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - colour.red()) <= tolerance &&
                std::abs(pixel.green() - colour.green()) <= tolerance &&
                std::abs(pixel.blue() - colour.blue()) <= tolerance) {
                return true;
            }
        }
    }
    return false;
}

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

    QImage image(view.viewport()->size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    view.viewport()->render(&image);
    return image;
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
