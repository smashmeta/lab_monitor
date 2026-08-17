#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>

#include "lm/ui/sparkline.hpp"
#include "lm/ui/theme.hpp"
#include "pixel_probe.hpp"

using namespace lm::ui;

namespace {

/// Red minus green: strongly positive when a colour reads as hot, strongly
/// negative when it reads as cool. Lets the ramp be checked by direction
/// rather than by pinning exact interpolated values.
double warmth(const QColor& colour) {
    return colour.redF() - colour.greenF();
}

}  // namespace

TEST(ColorForLoad, EndsOnTheGreenAndRedTheRestOfTheWindowUses) {
    EXPECT_EQ(Theme::color_for_load(0.0), QColor(Theme::kOnline));
    EXPECT_EQ(Theme::color_for_load(100.0), QColor(Theme::kMissing));
}

TEST(ColorForLoad, PassesThroughAmberRatherThanOlive) {
    EXPECT_EQ(Theme::color_for_load(50.0), QColor(Theme::kOffline));
}

TEST(ColorForLoad, GetsWarmerAllTheWayUp) {
    double previous = warmth(Theme::color_for_load(0.0));
    for (double percent = 5.0; percent <= 100.0; percent += 5.0) {
        const double current = warmth(Theme::color_for_load(percent));
        EXPECT_GT(current, previous) << "load " << percent << "% is not warmer than the step below";
        previous = current;
    }
}

TEST(ColorForLoad, ClampsOutOfRangeReadings) {
    EXPECT_EQ(Theme::color_for_load(-30.0), QColor(Theme::kOnline));
    EXPECT_EQ(Theme::color_for_load(140.0), QColor(Theme::kMissing));
}

TEST(Sparkline, DefaultsToTheAccentRatherThanAMetricColour) {
    // Which metric this is showing is the caller's business. Defaulting to a
    // load ramp here would make a plain line chart into a CPU widget.
    const Sparkline sparkline;
    EXPECT_EQ(sparkline.color(), QColor(Theme::kAccent));
}

TEST(Sparkline, TakesItsColourFromTheLatestSampleOnceGivenARamp) {
    Sparkline sparkline;
    sparkline.set_color_ramp(&Theme::color_for_load);

    sparkline.push(2.0);
    EXPECT_EQ(sparkline.color(), Theme::color_for_load(2.0));

    sparkline.push(97.0);
    EXPECT_EQ(sparkline.color(), Theme::color_for_load(97.0))
        << "the colour has to follow the reading, not the first sample it saw";
    EXPECT_GT(warmth(sparkline.color()), 0.0) << "a saturated CPU should read hot";
}

TEST(Sparkline, AppliesANewRampToTheSampleAlreadyOnScreen) {
    Sparkline sparkline;
    sparkline.push(95.0);
    ASSERT_EQ(sparkline.color(), QColor(Theme::kAccent));

    sparkline.set_color_ramp(&Theme::color_for_load);

    EXPECT_EQ(sparkline.color(), Theme::color_for_load(95.0))
        << "the colour would otherwise be one interval late";
}

TEST(Sparkline, PaintsTheLineInTheRampColour) {
    Sparkline sparkline;
    sparkline.set_color_ramp(&Theme::color_for_load);
    sparkline.resize(160, 40);
    for (const double sample : {88.0, 92.0, 95.0, 91.0, 96.0}) {
        sparkline.push(sample);
    }
    sparkline.show();
    QApplication::processEvents();

    const QImage painted = lm::ui::test::paint(sparkline);
    EXPECT_TRUE(lm::ui::test::contains_colour(painted, Theme::color_for_load(96.0)));
    EXPECT_FALSE(lm::ui::test::contains_colour(painted, QColor(Theme::kAccent)))
        << "a CPU under load must not still be drawn in the idle default";
}

TEST(Sparkline, DroppingTheRampLeavesTheColourWhereItWas) {
    Sparkline sparkline;
    sparkline.set_color_ramp(&Theme::color_for_load);
    sparkline.push(90.0);
    const QColor hot = sparkline.color();

    sparkline.set_color_ramp({});
    sparkline.push(5.0);

    EXPECT_EQ(sparkline.color(), hot) << "without a ramp the colour is the caller's to set";
}

TEST(Sparkline, IgnoresAnInvalidColour) {
    Sparkline sparkline;
    sparkline.set_color(QColor(Theme::kOnline));
    sparkline.set_color(QColor{});
    EXPECT_EQ(sparkline.color(), QColor(Theme::kOnline));
}
