#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QTest>

#include "lm/ui/meter_bar.hpp"
#include "lm/ui/theme.hpp"
#include "pixel_probe.hpp"

using namespace lm::ui;

namespace {

/// Paints the bar once its fill animation has settled on `percent`.
QImage paint_settled(MeterBar& bar, double percent) {
    bar.resize(160, 16);
    bar.show();
    QApplication::processEvents();
    bar.set_value(percent);
    QTest::qWait(500);  // the fill animates over 300ms
    QApplication::processEvents();
    return lm::ui::test::paint(bar);
}

}  // namespace

TEST(MeterBar, FillsGreenWhenThereIsPlentyLeft) {
    MeterBar bar;
    EXPECT_TRUE(lm::ui::test::contains_colour(paint_settled(bar, 12.0), Theme::color_for_load(12.0)));
}

TEST(MeterBar, FillsRedWhenNearlyFull) {
    MeterBar bar;
    EXPECT_TRUE(lm::ui::test::contains_colour(paint_settled(bar, 97.0), Theme::color_for_load(97.0)));
}

TEST(MeterBar, NoLongerPaintsEveryReadingInTheAccent) {
    // It used to be blue until 90% and red after, which hid the whole climb and
    // made 89% and 91% look like different situations.
    MeterBar bar;
    EXPECT_FALSE(lm::ui::test::contains_colour(paint_settled(bar, 60.0), QColor(Theme::kAccent)));
}

TEST(MeterBar, ShowsTheReadingAsTextAsWellAsColour) {
    // Colour is never the only signal: the number is on the bar either way.
    MeterBar bar;
    const QImage painted = paint_settled(bar, 42.0);
    EXPECT_TRUE(lm::ui::test::contains_colour(painted, QColor(Theme::kText)));
}
