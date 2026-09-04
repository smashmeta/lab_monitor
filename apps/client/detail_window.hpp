#pragma once

#include <QCloseEvent>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QString>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <optional>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"
#include "lm/ui/adapter_list.hpp"
#include "lm/ui/rule_detail.hpp"
#include "lm/ui/meter_bar.hpp"
#include "lm/ui/sparkline.hpp"
#include "lm/ui/status_pill.hpp"

/// The window a user sees on left-click. Lives entirely on the GUI thread;
/// every slot here is fed by a queued connection from MonitorWorker's
/// signals, never by a direct call into the worker.
///
/// Quitting is deliberate rather than incidental: the title bar's close button
/// is disabled and the window carries its own Minimize and Close buttons, the
/// second of which asks first. Stopping a monitoring agent means the server
/// stops hearing from this machine, which is not something to do by reflex on
/// the way to getting a window off the screen.
namespace lm::ui {
class LogView;
}  // namespace lm::ui

class DetailWindow : public QWidget {
    Q_OBJECT

public:
    /// `elevated` overrides what this process's own token says, and exists
    /// only so the elevation banner can be tested both ways. Left unset --
    /// which is every production call -- it reads
    /// lm::platform::is_elevated(). Without the seam the banner's test
    /// asserts `visible == !is_elevated()`, which is satisfied by a window
    /// that never shows the banner at all whenever the suite is launched from
    /// an elevated shell.
    explicit DetailWindow(QString host_id, QWidget* parent = nullptr,
                          std::optional<bool> elevated = std::nullopt);

    /// Whether a system tray exists (main.cpp asks
    /// QSystemTrayIcon::isSystemTrayAvailable()). With one, Minimize hides to
    /// the tray and a close that slips past the disabled button — Alt+F4, the
    /// system menu — only hides. Without one this window is the app's entire
    /// UI, so Minimize has to leave a taskbar button behind and a close must
    /// really close.
    void set_tray_available(bool tray_available) { tray_available_ = tray_available; }

    /// Shows the log file path in the Log tab, with a button to open its
    /// folder. Set from main(), which is what resolved the path.
    void set_log_file_path(const QString& path);

signals:
    /// The user confirmed the Close button. Wired to QApplication::quit in
    /// main.cpp, the same place the tray's Quit action goes, so there is one
    /// shutdown path rather than two.
    void quit_requested();

public slots:
    void set_connected(int state);
    void apply_resources(lm::core::ResourceSample sample);
    void apply_report(lm::core::ComplianceReport report, QVector<lm::ui::RuleDetail> details);
    void set_applied_revision(quint64 revision);

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_minimize_clicked();
    /// Asks before quitting, then emits quit_requested().
    void on_close_clicked();

private:
    void sync_disk_bars(const std::vector<lm::core::DiskUsage>& disks);
    void rebuild_template_label();

    QLabel* hostname_label_;
    lm::ui::StatusPill* connection_pill_;
    QLabel* template_label_;
    /// Shown only while lm::platform::is_elevated() is false -- see
    /// apps/client/autostart.hpp. Constant for the process lifetime, same as
    /// the token it reads, so this is set once in the constructor rather than
    /// re-checked anywhere.
    QLabel* elevation_banner_;

    lm::ui::Sparkline* cpu_sparkline_;
    lm::ui::MeterBar* memory_bar_;
    QVBoxLayout* disk_layout_;
    /// One meter bar per mount point, keyed by DiskUsage::mount. Rebuilt to
    /// match whatever volumes the current sample reports.
    QMap<QString, lm::ui::MeterBar*> disk_bars_;
    lm::ui::AdapterList* adapter_list_;

    QTreeWidget* compliance_tree_;
    lm::ui::LogView* log_view_ = nullptr;

    QPushButton* minimize_button_;
    QPushButton* close_button_;

    QString host_id_;
    bool connected_ = false;
    quint64 applied_revision_ = 0;
    bool tray_available_ = true;
};
