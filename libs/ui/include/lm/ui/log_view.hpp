#pragma once

#include "lm/ui/export.hpp"

#include <QString>
#include <QWidget>

#include <memory>

class QCheckBox;
class QLabel;
class QTextEdit;

namespace lm::ui {

/// A live view of this process's spdlog output, for a Log tab or panel.
///
/// Deliberately spdlog-free in its interface. `lm_ui` links spdlog privately —
/// it is not part of any public header here — and exposing a sink type would
/// change that for every consumer. Everything spdlog is in the .cpp.
class LM_UI_EXPORT LogView : public QWidget {
    Q_OBJECT

public:
    explicit LogView(QWidget* parent = nullptr);

    /// Detaches first. The sink holds a raw pointer to this widget's text edit
    /// and posts to it from whatever thread logged, so a sink outliving the
    /// widget is a crash — and both apps log *after* deleting their window
    /// during shutdown, so that ordering is not hypothetical. Detaching here
    /// makes it impossible rather than merely unlikely.
    ~LogView() override;

    /// Adds a colour sink for this view to spdlog's default logger, and
    /// replays whatever the ring buffer already holds.
    ///
    /// The replay is the point: `configure_logging()` runs before any window
    /// exists, so a live sink alone would start at whatever happened to be
    /// logged after the widget was built — missing the entire startup banner,
    /// which is the half worth reading. Finds the ring buffer among the
    /// default logger's own sinks, so nothing has to be threaded through
    /// `main()`.
    void attach_to_default_logger(int max_lines = 5000);

    /// Removes this view's sink from the default logger. Idempotent, and safe
    /// to call when never attached.
    void detach_from_default_logger();

    /// Shows the path with a button that opens its folder. Omit for a view
    /// with no file behind it (the shopping cart logs to screen only).
    void set_log_file_path(const QString& path);

private:
    void copy_to_clipboard();

    QTextEdit* text_;
    QLabel* path_label_;
    QCheckBox* follow_;
    QString log_file_path_;
    /// Type-erased so this header needs no spdlog include; the .cpp knows.
    std::shared_ptr<void> sink_;
};

}  // namespace lm::ui
