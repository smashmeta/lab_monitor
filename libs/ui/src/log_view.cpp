#include "lm/ui/log_view.hpp"

#include <algorithm>
#include <memory>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

#include <spdlog/sinks/qt_sinks.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/spdlog.h>

#include "lm/ui/theme.hpp"

namespace lm::ui {
namespace {

/// The ring buffer `configure_logging()` installs, if this application has one.
std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> find_ring_buffer() {
    const auto logger = spdlog::default_logger();
    if (!logger) {
        return nullptr;
    }
    for (const spdlog::sink_ptr& sink : logger->sinks()) {
        if (auto ring = std::dynamic_pointer_cast<spdlog::sinks::ringbuffer_sink_mt>(sink)) {
            return ring;
        }
    }
    return nullptr;
}

}  // namespace

LogView::LogView(QWidget* parent)
    : QWidget(parent),
      text_(new QTextEdit(this)),
      path_label_(new QLabel(this)),
      follow_(new QCheckBox(QStringLiteral("Follow"), this)) {
    auto* root = new QVBoxLayout(this);

    text_->setReadOnly(true);
    text_->setUndoRedoEnabled(false);
    // No wrapping: a log line is a record, and wrapping turns a scan down the
    // timestamp column into a hunt. It scrolls sideways instead.
    text_->setLineWrapMode(QTextEdit::NoWrap);
    // The fixed-width system font, for the same reason the shopping cart's
    // paths pane uses one: in a proportional face the underscore and dot of
    // a path merge into one stroke, and columns of timestamps stop lining up.
    text_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    root->addWidget(text_, 1);

    auto* controls = new QHBoxLayout();
    path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path_label_->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::kTextMuted));
    controls->addWidget(path_label_, 1);

    follow_->setChecked(true);
    follow_->setToolTip(
        QStringLiteral("Scroll to the newest line as it arrives.\nUncheck to read back without "
                       "being dragged to the bottom."));
    controls->addWidget(follow_);

    auto* copy = new QPushButton(QStringLiteral("Copy"), this);
    copy->setToolTip(QStringLiteral("Copy the whole view to the clipboard."));
    connect(copy, &QPushButton::clicked, this, &LogView::copy_to_clipboard);
    controls->addWidget(copy);

    auto* clear = new QPushButton(QStringLiteral("Clear"), this);
    // Clears the view only. Said in the tooltip because a Clear button next to
    // a file path reads like it might delete the file.
    clear->setToolTip(QStringLiteral("Clear this view. The log file is not touched."));
    connect(clear, &QPushButton::clicked, text_, &QTextEdit::clear);
    controls->addWidget(clear);

    root->addLayout(controls);

    // Follow means "stay at the bottom", so it only has to act when the view
    // grows. Checking maximum() rather than remembering a flag keeps it honest
    // if the user scrolls while lines are arriving.
    connect(text_->document(), &QTextDocument::contentsChanged, this, [this] {
        if (follow_->isChecked()) {
            text_->verticalScrollBar()->setValue(text_->verticalScrollBar()->maximum());
        }
    });
}

LogView::~LogView() { detach_from_default_logger(); }

void LogView::attach_to_default_logger(int max_lines) {
    const auto logger = spdlog::default_logger();
    if (!logger || sink_ != nullptr) {
        return;
    }

    // Replay before attaching, so a line logged between the two cannot appear
    // twice — the ring buffer holds it and the live sink would too.
    if (const auto ring = find_ring_buffer()) {
        for (const std::string& line : ring->last_formatted()) {
            QString text = QString::fromStdString(line);
            while (text.endsWith(QChar('\n')) || text.endsWith(QChar('\r'))) {
                text.chop(1);
            }
            text_->append(text);
        }
    }

    // dark_colors=false gives the bright palette, which is the right way round
    // here: Theme::apply() installs a dark background, and the "dark" set is
    // meant for light backgrounds.
    auto sink = std::make_shared<spdlog::sinks::qt_color_sink_mt>(text_, max_lines,
                                                                  /*dark_colors=*/false,
                                                                  /*is_utf8=*/true);
    // The %^ and %$ markers are load-bearing, not decoration. qt_color_sink
    // colours the range the *formatter* marked on the message, and without the
    // markers that range is left over from whichever sink formatted last --
    // the console one, whose pattern does carry them. The result is real: the
    // level colour landed midway through a word and again on a host id,
    // because the offsets belonged to a different string.
    sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    logger->sinks().push_back(sink);
    sink_ = sink;
}

void LogView::detach_from_default_logger() {
    if (sink_ == nullptr) {
        return;
    }
    if (const auto logger = spdlog::default_logger()) {
        auto& sinks = logger->sinks();
        std::erase_if(sinks, [this](const spdlog::sink_ptr& sink) { return sink == sink_; });
    }
    sink_.reset();
}

void LogView::set_log_file_path(const QString& path) {
    log_file_path_ = path;
    path_label_->setText(path);
    path_label_->setToolTip(path);

    auto* open = new QPushButton(QStringLiteral("Open folder"), this);
    open->setToolTip(QStringLiteral("Show the log file in the file manager."));
    connect(open, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(log_file_path_).absolutePath()));
    });
    // Into the controls row, before Follow, so the two buttons that act on the
    // file sit together at the left with the path they refer to.
    if (auto* controls = qobject_cast<QHBoxLayout*>(layout()->itemAt(1)->layout())) {
        controls->insertWidget(1, open);
    }
}

void LogView::copy_to_clipboard() {
    if (QClipboard* clipboard = QApplication::clipboard()) {
        clipboard->setText(text_->toPlainText());
    }
}

}  // namespace lm::ui
