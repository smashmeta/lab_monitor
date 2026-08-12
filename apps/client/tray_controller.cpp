#include "tray_controller.hpp"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include <algorithm>
#include <utility>

#include "detail_window.hpp"
#include "lm/transport/messages.hpp"
#include "lm/ui/theme.hpp"

namespace {

/// A simple recolourable ring. "{{COLOR}}" is substituted per state rather
/// than shipping four separate assets, per the brief's "SVG template
/// recoloured by state" instruction.
constexpr const char* kIconSvgTemplate =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">)"
    R"(<circle cx="16" cy="16" r="12" fill="none" stroke="{{COLOR}}" stroke-width="5"/>)"
    R"(</svg>)";

QString substitute_color(const QColor& color) {
    QString svg(kIconSvgTemplate);
    return svg.replace(QStringLiteral("{{COLOR}}"), color.name());
}

QIcon render_ring_icon(const QColor& color) {
    QSvgRenderer renderer(substitute_color(color).toUtf8());
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    painter.end();
    return QIcon(pixmap);
}

double worst_disk_percent(const lm::core::ResourceSample& sample) {
    double worst = 0.0;
    for (const lm::core::DiskUsage& disk : sample.disks) {
        worst = std::max(worst, disk.used_percent());
    }
    return worst;
}

}  // namespace

TrayController::TrayController(QString host_id, DetailWindow* window, QObject* parent)
    : QObject(parent),
      menu_(),
      open_action_(menu_.addAction(QStringLiteral("Open"))),
      pause_action_(menu_.addAction(QStringLiteral("Pause reporting"))),
      copy_diagnostics_action_(menu_.addAction(QStringLiteral("Copy diagnostics"))),
      quit_action_(menu_.addAction(QStringLiteral("Quit"))),
      window_(window),
      host_id_(std::move(host_id)) {
    pause_action_->setCheckable(true);

    connect(open_action_, &QAction::triggered, this, [this] {
        if (window_->isVisible()) {
            window_->hide();
        } else {
            window_->show();
            window_->raise();
            window_->activateWindow();
        }
    });
    connect(pause_action_, &QAction::toggled, this,
            [this](bool paused) { emit reporting_paused_changed(paused); });
    connect(copy_diagnostics_action_, &QAction::triggered, this, &TrayController::copy_diagnostics);
    connect(quit_action_, &QAction::triggered, this, [this] { emit quit_requested(); });

    tray_.setContextMenu(&menu_);
    connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason != QSystemTrayIcon::Trigger) {
            return;
        }
        if (window_->isVisible()) {
            window_->hide();
        } else {
            window_->show();
            window_->raise();
            window_->activateWindow();
        }
    });

    rebuild_icon();
    rebuild_tooltip();
    tray_.show();
}

void TrayController::set_connected(int state) {
    connected_ = static_cast<lm::transport::ConnectionState>(state) == lm::transport::ConnectionState::Connected;
    rebuild_icon();
}

void TrayController::apply_resources(lm::core::ResourceSample sample) {
    last_resources_ = std::move(sample);
    rebuild_tooltip();
}

void TrayController::apply_report(lm::core::ComplianceReport report) {
    last_report_ = std::move(report);
    rebuild_icon();
}

void TrayController::set_applied_revision(quint64 revision) { applied_revision_ = revision; }

TrayController::Status TrayController::status() const {
    if (!connected_) {
        return Status::Disconnected;
    }
    if (lm::core::count_by_status(last_report_, lm::core::CheckStatus::Fail) > 0) {
        return Status::Failing;
    }
    if (lm::core::count_by_status(last_report_, lm::core::CheckStatus::Error) > 0) {
        return Status::Warning;
    }
    return Status::Connected;
}

QIcon TrayController::icon_for(Status status) const {
    switch (status) {
        case Status::Connected:
            return render_ring_icon(QColor(lm::ui::Theme::kOnline));
        case Status::Warning:
            return render_ring_icon(QColor(lm::ui::Theme::kOffline));
        case Status::Failing:
            return render_ring_icon(QColor(lm::ui::Theme::kMissing));
        case Status::Disconnected:
            return render_ring_icon(QColor(lm::ui::Theme::kNotApplicable));
    }
    return render_ring_icon(QColor(lm::ui::Theme::kNotApplicable));
}

void TrayController::rebuild_icon() { tray_.setIcon(icon_for(status())); }

void TrayController::rebuild_tooltip() {
    tray_.setToolTip(QStringLiteral("%1 — CPU %2% · Disk %3%")
                          .arg(host_id_)
                          .arg(last_resources_.cpu_percent, 0, 'f', 0)
                          .arg(worst_disk_percent(last_resources_), 0, 'f', 0));
}

void TrayController::copy_diagnostics() const {
    const QString text = QStringLiteral(
                              "Host: %1\n"
                              "Connection: %2\n"
                              "Applied revision: %3\n"
                              "CPU: %4%\n"
                              "Memory used: %5 / %6 bytes\n"
                              "Compliance: %7 pass, %8 fail, %9 error, %10 n/a\n")
                              .arg(host_id_)
                              .arg(connected_ ? QStringLiteral("connected") : QStringLiteral("disconnected"))
                              .arg(applied_revision_)
                              .arg(last_resources_.cpu_percent, 0, 'f', 1)
                              .arg(last_resources_.mem_used_bytes)
                              .arg(last_resources_.mem_total_bytes)
                              .arg(lm::core::count_by_status(last_report_, lm::core::CheckStatus::Pass))
                              .arg(lm::core::count_by_status(last_report_, lm::core::CheckStatus::Fail))
                              .arg(lm::core::count_by_status(last_report_, lm::core::CheckStatus::Error))
                              .arg(lm::core::count_by_status(last_report_, lm::core::CheckStatus::NotApplicable));
    QApplication::clipboard()->setText(text);
}
