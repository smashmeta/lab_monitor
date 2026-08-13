#pragma once

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QString>
#include <QSystemTrayIcon>

#include <cstdint>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"

class DetailWindow;

/// Tray-side presentation. Lives on the GUI thread. Caches just enough of the
/// worker's last-known state (via its public slots, connected with a queued
/// connection from MonitorWorker's signals) to paint its icon/tooltip and to
/// answer "Copy diagnostics" without reaching back across threads.
class TrayController : public QObject {
    Q_OBJECT

public:
    /// window is shown/hidden in response to tray activation; it is not owned.
    /// host_id is read once via lm::platform::local_host_name() in main() --
    /// a hostname lookup, not registry/service/DDS probing, so it is fine on
    /// the GUI thread -- and passed in here rather than looked up again.
    TrayController(QString host_id, DetailWindow* window, QObject* parent = nullptr);

public slots:
    void set_connected(int state);
    void apply_resources(lm::core::ResourceSample sample);
    void apply_report(lm::core::ComplianceReport report);
    void set_applied_revision(quint64 revision);

signals:
    /// Emitted when the user toggles "Pause reporting" from the context menu.
    /// Connect this (queued) to MonitorWorker::set_reporting_paused.
    void reporting_paused_changed(bool paused);
    /// Emitted when the user picks "Quit" from the context menu.
    void quit_requested();

private:
    enum class Status { Connected, Warning, Failing, Disconnected };

    void rebuild_icon();
    void rebuild_tooltip();
    [[nodiscard]] Status status() const;
    [[nodiscard]] QIcon icon_for(Status status) const;
    void copy_diagnostics() const;

    QSystemTrayIcon tray_;
    QMenu menu_;
    QAction* open_action_;
    QAction* pause_action_;
    QAction* copy_diagnostics_action_;
    QAction* quit_action_;
    DetailWindow* window_;

    QString host_id_;
    bool connected_ = false;
    lm::core::ResourceSample last_resources_;
    lm::core::ComplianceReport last_report_;
    quint64 applied_revision_ = 0;
};
