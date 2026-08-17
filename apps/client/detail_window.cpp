#include "detail_window.hpp"

#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QSet>
#include <QTreeWidgetItem>

#include <utility>
#include <vector>

#include "lm/core/fleet.hpp"
#include "lm/transport/messages.hpp"
#include "lm/ui/theme.hpp"
#include "lm/ui/keep_foreground_delegate.hpp"

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
      cpu_sparkline_(new lm::ui::Sparkline(this)),
      memory_bar_(new lm::ui::MeterBar(this)),
      disk_layout_(new QVBoxLayout()),
      compliance_tree_(new QTreeWidget(this)),
      host_id_(std::move(host_id)) {
    setWindowTitle(hostname_label_->text());
    resize(480, 560);

    auto* root = new QVBoxLayout(this);

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
    root->addLayout(header);

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
    root->addLayout(resources);

    // Compliance list, grouped and dimmed per the brief's header comment.
    // Keeps each row's status colour when selected, instead of the style
    // repainting it with QPalette::HighlightedText.
    compliance_tree_->setItemDelegate(new lm::ui::KeepForegroundDelegate(compliance_tree_));
    compliance_tree_->setColumnCount(3);
    compliance_tree_->setHeaderLabels(
        {QStringLiteral("Rule"), QStringLiteral("Status"), QStringLiteral("Observed")});
    root->addWidget(compliance_tree_, 1);

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
}

void DetailWindow::set_applied_revision(quint64 revision) {
    applied_revision_ = revision;
    rebuild_template_label();
}

void DetailWindow::closeEvent(QCloseEvent* event) {
    if (hide_on_close_) {
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
