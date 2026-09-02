#include "detail_window.hpp"

#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QMessageBox>
#include <QSet>
#include <QTreeWidgetItem>

#include <algorithm>
#include <utility>
#include <vector>

#include "lm/core/fleet.hpp"
#include "lm/platform/probes.hpp"
#include "lm/transport/messages.hpp"
#include "lm/ui/theme.hpp"
#include "lm/ui/keep_foreground_delegate.hpp"
#include "lm/ui/log_view.hpp"

namespace {

/// One row per resource, label to the left, gauge to the right, used both for
/// the fixed memory row and for each dynamically-added disk row.
QWidget* make_gauge_row(QWidget* parent, const QString& label_text, QWidget* gauge) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(label_text, row);
    label->setMinimumWidth(80);
    layout->addWidget(label);
    layout->addWidget(gauge, 1);
    return row;
}

}  // namespace

DetailWindow::DetailWindow(QString host_id, QWidget* parent)
    : QWidget(parent),
      hostname_label_(new QLabel(host_id, this)),
      connection_pill_(new lm::ui::StatusPill(this)),
      template_label_(new QLabel(this)),
      elevation_banner_(new QLabel(this)),
      cpu_sparkline_(new lm::ui::Sparkline(this)),
      memory_bar_(new lm::ui::MeterBar(this)),
      disk_layout_(new QVBoxLayout()),
      adapter_list_(new lm::ui::AdapterList(this)),
      compliance_tree_(new QTreeWidget(this)),
      minimize_button_(new QPushButton(QStringLiteral("Hide"), this)),
      close_button_(new QPushButton(QStringLiteral("Close Program"), this)),
      host_id_(std::move(host_id)) {
    setWindowTitle(hostname_label_->text());
    // Wider than it was: a log line is a timestamp, a level and a sentence,
    // and the Details tab was the only thing sizing this window before.
    resize(720, 620);

    // No close button in the title bar: quitting a monitoring agent is done
    // through the Close Program button below, which asks first. Windows draws
    // the X regardless and only greys it out -- disabled is as far as the
    // platform goes -- so closeEvent() still has to handle Alt+F4 and the
    // system menu.
    setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                   Qt::WindowSystemMenuHint | Qt::WindowMinimizeButtonHint);

    auto* root = new QVBoxLayout(this);

    // Two tabs, with Minimize and Close Program left *outside* them: those are
    // window controls, not part of either view, and a Close Program button
    // that disappears when you switch tabs is a button somebody cannot find.
    auto* tabs = new QTabWidget(this);
    auto* details = new QWidget(tabs);
    auto* details_layout = new QVBoxLayout(details);
    root->addWidget(tabs, 1);

    // Header band: hostname, connection pill, applied template revision.
    auto* header = new QHBoxLayout();
    QFont hostname_font = hostname_label_->font();
    hostname_font.setBold(true);
    hostname_font.setPointSize(hostname_font.pointSize() + 2);
    hostname_label_->setFont(hostname_font);
    header->addWidget(hostname_label_);
    header->addWidget(connection_pill_);
    header->addStretch();
    header->addWidget(template_label_);
    details_layout->addLayout(header);

    // Said on screen as well as in the log, because the person who will hit
    // this is the one looking at the machine after a script failed on it.
    elevation_banner_->setObjectName(QStringLiteral("ElevationBanner"));
    elevation_banner_->setText(
        QStringLiteral("Not running elevated — scripts that install or uninstall "
                       "will fail. Run --install-autostart from an administrator "
                       "prompt to fix this."));
    elevation_banner_->setWordWrap(true);
    elevation_banner_->setStyleSheet(QStringLiteral("color: %1;").arg(lm::ui::Theme::kOffline));
    elevation_banner_->setVisible(!lm::platform::is_elevated());
    details_layout->addWidget(elevation_banner_);

    // Resource strip: CPU sparkline, memory bar, one bar per disk volume.
    auto* resources = new QVBoxLayout();
    resources->addWidget(new QLabel(QStringLiteral("CPU"), this));
    // Green when idle through to red when saturated, so the line reports the
    // current load by colour as well as by height -- which matters here,
    // because the chart's vertical axis auto-scales to the window it is showing
    // and so says nothing about the absolute level on its own.
    cpu_sparkline_->set_color_ramp(&lm::ui::Theme::color_for_load);
    resources->addWidget(cpu_sparkline_);
    resources->addWidget(make_gauge_row(this, QStringLiteral("Memory"), memory_bar_));
    resources->addLayout(disk_layout_);
    details_layout->addLayout(resources);

    details_layout->addWidget(new QLabel(QStringLiteral("Network adapters"), this));
    details_layout->addWidget(adapter_list_, 1);

    // Compliance list, grouped and dimmed per the brief's header comment.
    // Keeps each row's status colour when selected, instead of the style
    // repainting it with QPalette::HighlightedText.
    compliance_tree_->setItemDelegate(new lm::ui::KeepForegroundDelegate(compliance_tree_));
    compliance_tree_->setColumnCount(3);
    compliance_tree_->setHeaderLabels(
        {QStringLiteral("Rule"), QStringLiteral("Status"), QStringLiteral("Observed")});
    details_layout->addWidget(compliance_tree_, 1);

    tabs->addTab(details, QStringLiteral("Details"));
    log_view_ = new lm::ui::LogView(tabs);
    log_view_->attach_to_default_logger();
    tabs->addTab(log_view_, QStringLiteral("Log"));

    // Object names so tests (and any future QSS) can find these by role. The
    // labels are wording, free to change; what they do is not.
    minimize_button_->setObjectName(QStringLiteral("MinimizeButton"));
    close_button_->setObjectName(QStringLiteral("CloseButton"));

    // Right-aligned, with Close last: the destructive one sits where the eye
    // stops rather than where the cursor lands on the way past.
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    buttons->addWidget(minimize_button_);
    buttons->addWidget(close_button_);
    root->addLayout(buttons);

    connect(minimize_button_, &QPushButton::clicked, this, &DetailWindow::on_minimize_clicked);
    connect(close_button_, &QPushButton::clicked, this, &DetailWindow::on_close_clicked);

    rebuild_template_label();
    set_connected(static_cast<int>(lm::transport::ConnectionState::Disconnected));
}

void DetailWindow::set_connected(int state) {
    connected_ = static_cast<lm::transport::ConnectionState>(state) == lm::transport::ConnectionState::Connected;
    connection_pill_->set_state(connected_ ? lm::core::HostState::Online : lm::core::HostState::Offline);
}

void DetailWindow::apply_resources(lm::core::ResourceSample sample) {
    cpu_sparkline_->push(sample.cpu_percent);

    const double mem_percent = sample.mem_total_bytes == 0
                                    ? 0.0
                                    : 100.0 * static_cast<double>(sample.mem_used_bytes) /
                                          static_cast<double>(sample.mem_total_bytes);
    memory_bar_->set_value(mem_percent);

    sync_disk_bars(sample.disks);
    adapter_list_->set_adapters(sample.adapters);
}

namespace {

QString status_name(lm::core::CheckStatus status) {
    switch (status) {
        case lm::core::CheckStatus::Pass:          return QStringLiteral("Pass");
        case lm::core::CheckStatus::Fail:          return QStringLiteral("Fail");
        case lm::core::CheckStatus::Error:         return QStringLiteral("Error");
        case lm::core::CheckStatus::NotApplicable: return QStringLiteral("Not applicable");
    }
    return QStringLiteral("Unknown");
}

}  // namespace

void DetailWindow::apply_report(lm::core::ComplianceReport report, QVector<lm::ui::RuleDetail> details) {
    compliance_tree_->clear();

    // core::CheckResult carries only a rule id, so the rule's description and
    // payload arrive alongside it via report_ready's second argument, built by
    // MonitorWorker from the bundle it holds. Index them by id for lookup.
    QHash<QString, lm::ui::RuleDetail> by_id;
    by_id.reserve(details.size());
    for (const lm::ui::RuleDetail& detail : details) {
        by_id.insert(detail.id, detail);
    }

    // Grouped by CheckStatus rather than by RuleKind. Now that `details`
    // carries each rule's kind, grouping by Applications / Services / Registry
    // is a small change -- but status-first is what an operator needs to see
    // first: failures, then errors, then passes, with NotApplicable rows
    // present but dimmed exactly as specified.
    struct Group {
        const char* title;
        lm::core::CheckStatus status;
    };
    static constexpr Group kGroups[] = {
        {"Failing", lm::core::CheckStatus::Fail},
        {"Errors", lm::core::CheckStatus::Error},
        {"Passing", lm::core::CheckStatus::Pass},
        {"Not Applicable", lm::core::CheckStatus::NotApplicable},
    };

    for (const Group& group : kGroups) {
        std::vector<const lm::core::CheckResult*> matches;
        for (const lm::core::CheckResult& result : report.results) {
            if (result.status == group.status) {
                matches.push_back(&result);
            }
        }
        if (matches.empty()) {
            continue;
        }

        auto* header = new QTreeWidgetItem(compliance_tree_);
        header->setText(0, QStringLiteral("%1 (%2)").arg(group.title).arg(matches.size()));
        header->setFirstColumnSpanned(true);

        for (const lm::core::CheckResult* result : matches) {
            auto* row = new QTreeWidgetItem(header);

            const QString id = QString::fromStdString(result->rule_id);
            const auto detail = by_id.constFind(id);
            const bool described = detail != by_id.constEnd();

            // Show the authored description. Fall back to the id only if this
            // result has no matching rule at all, which means the bundle
            // changed between evaluating and rendering.
            row->setText(0, described ? detail->label : id);
            row->setText(1, lm::ui::Theme::glyph_for(result->status));

            // Both are populated for an error; observed alone is often terse.
            QString observed = QString::fromStdString(result->observed);
            const QString message = QString::fromStdString(result->message);
            if (!message.isEmpty() && message != observed) {
                observed = observed.isEmpty() ? message
                                              : QStringLiteral("%1 - %2").arg(observed, message);
            }
            row->setText(2, observed);

            QString tooltip = described ? detail->tooltip() : QStringLiteral("Rule id: %1").arg(id);
            tooltip += QStringLiteral("\n\nResult:\t%1").arg(status_name(result->status));
            if (!observed.isEmpty()) {
                tooltip += QStringLiteral("\nObserved:\t%1").arg(observed);
            }

            const QColor color = result->status == lm::core::CheckStatus::NotApplicable
                                      ? QColor(lm::ui::Theme::kTextMuted)
                                      : lm::ui::Theme::color_for(result->status);
            for (int column = 0; column < 3; ++column) {
                row->setForeground(column, color);
                row->setToolTip(column, tooltip);
            }
        }
        header->setExpanded(true);
    }

    // Same reasoning as the server's pane: a truncated "Wire…" defeats the
    // point of labelling rows with the description instead of the rule id.
    compliance_tree_->resizeColumnToContents(0);
    constexpr int kMaxRuleWidth = 260;
    compliance_tree_->setColumnWidth(0,
                                     std::min(compliance_tree_->columnWidth(0), kMaxRuleWidth));
}

void DetailWindow::set_applied_revision(quint64 revision) {
    applied_revision_ = revision;
    rebuild_template_label();
}

void DetailWindow::on_minimize_clicked() {
    // To the tray when there is one -- the tray icon is then how it comes
    // back, and leaving a taskbar button as well would be two of the same
    // thing. Without a tray, hiding would strand the user with no way to
    // reach the window again, so it minimises normally instead.
    if (tray_available_) {
        hide();
    } else {
        showMinimized();
    }
}

void DetailWindow::on_close_clicked() {
    // Names the consequence rather than asking "are you sure?": what matters
    // is that the server stops hearing from this machine, not that a window
    // is about to disappear.
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("Close Lab Monitor"),
        QStringLiteral("Close Lab Monitor on %1?\n\n"
                        "Monitoring stops and this machine stops reporting to the server "
                        "until the client is started again.")
            .arg(host_id_),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer == QMessageBox::Yes) {
        emit quit_requested();
    }
}

void DetailWindow::set_log_file_path(const QString& path) { log_view_->set_log_file_path(path); }

void DetailWindow::closeEvent(QCloseEvent* event) {
    // Reached by Alt+F4 and the system menu, since the platform only greys the
    // title bar's X rather than removing it. With a tray this hides, matching
    // Minimize; without one the window is the whole app and must really close.
    if (tray_available_) {
        hide();
        event->ignore();
        return;
    }
    QWidget::closeEvent(event);
}

void DetailWindow::sync_disk_bars(const std::vector<lm::core::DiskUsage>& disks) {
    QSet<QString> seen;
    for (const lm::core::DiskUsage& disk : disks) {
        const QString mount = QString::fromStdString(disk.mount);
        seen.insert(mount);

        auto it = disk_bars_.find(mount);
        lm::ui::MeterBar* bar = nullptr;
        if (it == disk_bars_.end()) {
            bar = new lm::ui::MeterBar(this);
            disk_layout_->addWidget(make_gauge_row(this, mount, bar));
            disk_bars_.insert(mount, bar);
        } else {
            bar = it.value();
        }
        bar->set_value(disk.used_percent());
    }

    for (auto it = disk_bars_.begin(); it != disk_bars_.end();) {
        if (seen.contains(it.key())) {
            ++it;
        } else {
            it.value()->parentWidget()->deleteLater();
            it = disk_bars_.erase(it);
        }
    }
}

void DetailWindow::rebuild_template_label() {
    template_label_->setText(applied_revision_ == 0
                                  ? QStringLiteral("No template applied")
                                  : QStringLiteral("Revision %1").arg(applied_revision_));
}
