#pragma once

#include <QCloseEvent>
#include <QLabel>
#include <QMap>
#include <QString>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"
#include "rule_detail.hpp"
#include "lm/ui/meter_bar.hpp"
#include "lm/ui/sparkline.hpp"
#include "lm/ui/status_pill.hpp"

/// The window a user sees on left-click. Lives entirely on the GUI thread;
/// every slot here is fed by a queued connection from MonitorWorker's
/// signals, never by a direct call into the worker.
///
/// By default, closing the window only hides it -- see closeEvent() -- so
/// the tray remains the only way to quit. set_hide_on_close(false) switches
/// to ordinary close-means-close behaviour, for when there is no tray (see
/// main.cpp's QSystemTrayIcon::isSystemTrayAvailable() guard) and this
/// window is therefore the app's only way to quit.
class DetailWindow : public QWidget {
    Q_OBJECT

public:
    explicit DetailWindow(QString host_id, QWidget* parent = nullptr);

    /// true (the default) preserves the tray-app behaviour of hiding
    /// instead of closing; false lets the window actually close, ending the
    /// app if quitOnLastWindowClosed is also set.
    void set_hide_on_close(bool hide_on_close) { hide_on_close_ = hide_on_close; }

public slots:
    void set_connected(int state);
    void apply_resources(lm::core::ResourceSample sample);
    void apply_report(lm::core::ComplianceReport report, QVector<RuleDetail> details);
    void set_applied_revision(quint64 revision);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void sync_disk_bars(const std::vector<lm::core::DiskUsage>& disks);
    void rebuild_template_label();

    QLabel* hostname_label_;
    lm::ui::StatusPill* connection_pill_;
    QLabel* template_label_;

    lm::ui::Sparkline* cpu_sparkline_;
    lm::ui::MeterBar* memory_bar_;
    QVBoxLayout* disk_layout_;
    /// One meter bar per mount point, keyed by DiskUsage::mount. Rebuilt to
    /// match whatever volumes the current sample reports.
    QMap<QString, lm::ui::MeterBar*> disk_bars_;

    QTreeWidget* compliance_tree_;

    QString host_id_;
    bool connected_ = false;
    quint64 applied_revision_ = 0;
    bool hide_on_close_ = true;
};
