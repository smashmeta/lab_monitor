#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "detail_window.hpp"
#include "lm/platform/probes.hpp"

namespace {

/// By object name, not by label: the wording on these buttons is presentation
/// and has already been changed once, which turned every lookup here into a
/// null dereference. What the button *does* is the stable thing.
QPushButton* button_named(DetailWindow& window, const QString& object_name) {
    return window.findChild<QPushButton*>(object_name);
}

QPushButton* minimize_button(DetailWindow& window) {
    QPushButton* button = button_named(window, QStringLiteral("MinimizeButton"));
    EXPECT_NE(button, nullptr) << "no button named MinimizeButton";
    return button;
}

QPushButton* close_button(DetailWindow& window) {
    QPushButton* button = button_named(window, QStringLiteral("CloseButton"));
    EXPECT_NE(button, nullptr) << "no button named CloseButton";
    return button;
}

/// Answers the next modal question box. Queued so it runs inside the dialog's
/// own event loop -- QMessageBox::question blocks until something clicks it,
/// so the answer has to be scheduled before the click that opens it.
void answer_next_question(QMessageBox::StandardButton answer) {
    QTimer::singleShot(0, [answer] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<QMessageBox*>(widget)) {
                box->button(answer)->click();
                return;
            }
        }
        ADD_FAILURE() << "no question box was open to answer";
    });
}

}  // namespace

TEST(DetailWindow, HasNoCloseButtonInItsTitleBar) {
    const DetailWindow window(QStringLiteral("PC-001"));
    EXPECT_FALSE(window.windowFlags().testFlag(Qt::WindowCloseButtonHint));
    EXPECT_TRUE(window.windowFlags().testFlag(Qt::WindowTitleHint))
        << "the title bar itself should stay";
}

TEST(DetailWindow, CarriesItsOwnMinimizeAndCloseButtons) {
    DetailWindow window(QStringLiteral("PC-001"));
    EXPECT_NE(minimize_button(window), nullptr);
    EXPECT_NE(close_button(window), nullptr);
}

TEST(DetailWindow, MinimizeHidesToTheTrayWhenThereIsOne) {
    DetailWindow window(QStringLiteral("PC-001"));
    window.set_tray_available(true);
    window.show();
    QApplication::processEvents();
    ASSERT_TRUE(window.isVisible());

    minimize_button(window)->click();
    QApplication::processEvents();

    EXPECT_FALSE(window.isVisible()) << "the tray icon is how it comes back";
}

TEST(DetailWindow, MinimizeLeavesAWindowBehindWhenThereIsNoTray) {
    // Hiding with no tray would strand the user: there would be nothing left
    // to click to get the window back.
    DetailWindow window(QStringLiteral("PC-001"));
    window.set_tray_available(false);
    window.show();
    QApplication::processEvents();

    minimize_button(window)->click();
    QApplication::processEvents();

    EXPECT_TRUE(window.isMinimized());
}

TEST(DetailWindow, AsksBeforeClosingAndQuitsOnlyIfConfirmed) {
    DetailWindow window(QStringLiteral("PC-001"));
    QSignalSpy spy(&window, &DetailWindow::quit_requested);
    ASSERT_TRUE(spy.isValid());

    answer_next_question(QMessageBox::Yes);
    close_button(window)->click();

    EXPECT_EQ(spy.count(), 1);
}

TEST(DetailWindow, DoesNotQuitWhenTheUserDeclines) {
    DetailWindow window(QStringLiteral("PC-001"));
    QSignalSpy spy(&window, &DetailWindow::quit_requested);

    answer_next_question(QMessageBox::No);
    close_button(window)->click();

    EXPECT_EQ(spy.count(), 0) << "declining the question must leave the client running";
}

TEST(DetailWindow, DefaultsTheQuestionToNo) {
    // A stray Return or Space must not take down a monitoring agent.
    DetailWindow window(QStringLiteral("PC-001"));
    QSignalSpy spy(&window, &DetailWindow::quit_requested);

    QTimer::singleShot(0, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* box = qobject_cast<QMessageBox*>(widget)) {
                EXPECT_EQ(box->defaultButton(), box->button(QMessageBox::No));
                QTest::keyClick(box, Qt::Key_Enter);
                return;
            }
        }
        ADD_FAILURE() << "no question box was open";
    });
    close_button(window)->click();

    EXPECT_EQ(spy.count(), 0);
}

TEST(DetailWindow, AltF4OnlyHidesWhileThereIsATray) {
    // The platform greys the title bar's X rather than removing it, so a close
    // can still arrive by other routes. It must not quit.
    DetailWindow window(QStringLiteral("PC-001"));
    window.set_tray_available(true);
    window.show();
    QApplication::processEvents();

    QSignalSpy spy(&window, &DetailWindow::quit_requested);
    window.close();
    QApplication::processEvents();

    EXPECT_FALSE(window.isVisible());
    EXPECT_EQ(spy.count(), 0);
}

TEST(DetailWindowElevation, ShowsTheBannerOnlyWhenNotElevated) {
    DetailWindow window(QStringLiteral("PC-001"));
    auto* banner = window.findChild<QLabel*>(QStringLiteral("ElevationBanner"));
    ASSERT_NE(banner, nullptr) << "no elevation banner";

    // The test process's own elevation decides which way this goes, so assert
    // the relationship rather than a fixed value.
    EXPECT_EQ(banner->isVisibleTo(&window), !lm::platform::is_elevated());
}
