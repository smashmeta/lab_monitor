#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <QApplication>
#include <QTextEdit>

#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include "lm/ui/log_view.hpp"

using namespace lm::ui;

namespace {

/// A default logger with the ring buffer LogView replays from, matching what
/// each application's configure_logging() installs.
void install_logger() {
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(100)};
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("test", sinks.begin(), sinks.end()));
    spdlog::set_level(spdlog::level::info);
}

QString text_of(const LogView& view) {
    // The sink writes into the view's own QTextEdit; finding it by type is
    // enough here and keeps LogView from having to expose it.
    const auto* edit = view.findChild<QTextEdit*>();
    return edit != nullptr ? edit->toPlainText() : QString{};
}

}  // namespace

TEST(LogView, ReplaysWhatWasLoggedBeforeItExisted) {
    // The point of the ring buffer: configure_logging() necessarily runs before
    // any window does, so a live sink alone would miss the startup banner --
    // which is the half worth reading.
    install_logger();
    spdlog::info("before the view existed");

    LogView view;
    view.attach_to_default_logger();
    QApplication::processEvents();

    EXPECT_NE(text_of(view).indexOf(QStringLiteral("before the view existed")), -1)
        << text_of(view).toStdString();
}

TEST(LogView, ShowsLinesLoggedAfterAttaching) {
    install_logger();
    LogView view;
    view.attach_to_default_logger();

    spdlog::info("after attaching");
    QApplication::processEvents();

    EXPECT_NE(text_of(view).indexOf(QStringLiteral("after attaching")), -1);
}

TEST(LogView, DoesNotShowALineTwiceWhenItIsBothBufferedAndLive) {
    // Replay happens before the live sink is added, precisely so a line cannot
    // arrive down both paths.
    install_logger();
    spdlog::info("only once please");

    LogView view;
    view.attach_to_default_logger();
    QApplication::processEvents();

    const QString text = text_of(view);
    EXPECT_EQ(text.count(QStringLiteral("only once please")), 1) << text.toStdString();
}

TEST(LogView, StopsReceivingOnceDetached) {
    install_logger();
    LogView view;
    view.attach_to_default_logger();
    view.detach_from_default_logger();

    spdlog::info("after detaching");
    QApplication::processEvents();

    EXPECT_EQ(text_of(view).indexOf(QStringLiteral("after detaching")), -1);
}

TEST(LogView, DetachingTwiceIsHarmless) {
    install_logger();
    LogView view;
    view.attach_to_default_logger();
    view.detach_from_default_logger();
    EXPECT_NO_THROW(view.detach_from_default_logger());
}

TEST(LogView, LoggingAfterTheViewIsDestroyedIsSafe) {
    // The one that matters. qt_color_sink holds a raw QTextEdit* and posts to
    // it from whatever thread logged, and both applications log *after*
    // deleting their window during shutdown -- so a sink outliving the widget
    // is not hypothetical. The destructor detaches, which is what makes this
    // impossible rather than merely unlikely.
    install_logger();
    {
        LogView view;
        view.attach_to_default_logger();
        spdlog::info("while alive");
    }

    EXPECT_EQ(spdlog::default_logger()->sinks().size(), 1u)
        << "the view's sink should have been removed with it";
    EXPECT_NO_THROW(spdlog::info("after the view is gone"));
    QApplication::processEvents();
}
