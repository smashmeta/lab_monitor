#include <gtest/gtest.h>

#include <QApplication>

#include "lm/ui/theme.hpp"

/// A QApplication rather than the QCoreApplication the other lm_ui tests use,
/// and Theme::apply() before the first test runs: these tests paint real
/// widgets through the real stylesheet, which is the only place a QSS rule
/// that overrides the model's own colours can be caught.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    lm::ui::Theme::apply(app);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
