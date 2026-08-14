#include "fleet_window.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStringList>
#include <QTableView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

#include "lm/core/rule.hpp"
#include "lm/ui/fleet_model.hpp"
#include "lm/ui/meter_bar.hpp"
#include "lm/ui/sparkline.hpp"
#include "lm/ui/theme.hpp"
#include "server_controller.hpp"
#include "status_ribbon.hpp"

namespace {

/// One row per resource, label to the left, gauge to the right. Mirrors the
/// client's detail_window.cpp helper of the same shape.
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

QString kind_label(lm::core::RuleKind kind) {
    switch (kind) {
        case lm::core::RuleKind::Process:  return QStringLiteral("Process");
        case lm::core::RuleKind::Service:  return QStringLiteral("Service");
        case lm::core::RuleKind::Registry: return QStringLiteral("Registry");
    }
    return QStringLiteral("Unknown");
}

QString target_label(const lm::core::Rule& rule) {
    return std::visit(
        [](const auto& payload) -> QString {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, lm::core::ProcessRule>) {
                return QString::fromStdString(payload.executable);
            } else if constexpr (std::is_same_v<T, lm::core::ServiceRule>) {
                return QString::fromStdString(payload.service_name);
            } else {
                return QString::fromStdString(lm::core::registry_key(payload));
            }
        },
        rule.payload);
}

}  // namespace

/// See fleet_window.hpp's forward-declaration comment: adds a HostState
/// filter and a stale-only filter on top of QSortFilterProxyModel's own
/// substring filter (still driven by the sidebar's QLineEdit via
/// setFilterFixedString). No new signals/slots/properties are declared, so
/// this deliberately has no Q_OBJECT and needs no moc handling at all.
class FleetProxyModel : public QSortFilterProxyModel {
public:
    explicit FleetProxyModel(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}

    void set_state_filter(std::optional<lm::core::HostState> state) {
        state_filter_ = state;
        invalidateFilter();
    }

    void set_stale_only(bool stale_only) {
        stale_only_ = stale_only;
        invalidateFilter();
    }

protected:
    [[nodiscard]] bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override {
        if (!QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent)) {
            return false;
        }
        const QModelIndex index = sourceModel()->index(source_row, 0, source_parent);
        if (state_filter_.has_value()) {
            const auto state =
                static_cast<lm::core::HostState>(sourceModel()->data(index, lm::ui::FleetModel::StateRole).toInt());
            if (state != *state_filter_) {
                return false;
            }
        }
        if (stale_only_ && !sourceModel()->data(index, lm::ui::FleetModel::StaleRole).toBool()) {
            return false;
        }
        return true;
    }

private:
    std::optional<lm::core::HostState> state_filter_;
    bool stale_only_ = false;
};

FleetWindow::FleetWindow(ServerController* controller, QWidget* parent)
    : QMainWindow(parent), controller_(controller) {
    setWindowTitle(QStringLiteral("Lab Monitor - Fleet Console"));
    resize(1100, 720);

    proxy_ = new FleetProxyModel(this);
    proxy_->setSourceModel(controller_->model());
    proxy_->setSortRole(lm::ui::FleetModel::SeverityRole);
    proxy_->setFilterKeyColumn(lm::ui::FleetModel::HostColumn);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);

    tabs_ = new QTabWidget(this);
    setCentralWidget(tabs_);

    build_fleet_tab();
    build_templates_tab();

    // The proxy sorts most-urgent-first without the source FleetModel ever
    // reordering its own rows (which stay stable, by host id, for apply()'s
    // insert/remove diffing to work).
    proxy_->sort(0);

    connect(controller_, &ServerController::counts_changed, ribbon_, &StatusRibbon::set_counts);
    connect(controller_, &ServerController::resource_sample_received, this, &FleetWindow::on_resource_sample);
    connect(controller_, &ServerController::compliance_report_received, this,
            &FleetWindow::on_compliance_report);
    // The Templates tab builds itself in this constructor, which runs before
    // ServerController::start() loads the persisted config. Without these the
    // tab would show an empty draft until the operator's first edit happened
    // to trigger a rebuild.
    connect(controller_, &ServerController::published_changed, this,
            &FleetWindow::rebuild_templates_view);
    connect(controller_, &ServerController::expected_hosts_changed, this,
            &FleetWindow::rebuild_templates_view);
    connect(controller_, &ServerController::draft_publishable_changed, this,
            &FleetWindow::on_draft_publishable_changed);
    connect(controller_, &ServerController::config_error, this, [this](const QString& message) {
        QMessageBox::warning(this, QStringLiteral("Configuration error"), message);
    });

    on_draft_publishable_changed(controller_->can_publish());
    refresh_detail_pane({});

    restore_window_state();
}

void FleetWindow::closeEvent(QCloseEvent* event) {
    save_window_state();
    QMainWindow::closeEvent(event);
}

void FleetWindow::build_fleet_tab() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    ribbon_ = new StatusRibbon(page);
    layout->addWidget(ribbon_);

    auto* filter_row = new QHBoxLayout();
    filter_edit_ = new QLineEdit(page);
    filter_edit_->setPlaceholderText(QStringLiteral("Filter by host..."));
    filter_row->addWidget(filter_edit_, 1);

    // Without these, clicking a ribbon counter silently changed which rows were
    // listed and gave no clue that a filter was on, what it was, or how to
    // undo it.
    filter_status_label_ = new QLabel(page);
    filter_status_label_->setProperty("muted", true);
    filter_row->addWidget(filter_status_label_);

    clear_filter_button_ = new QPushButton(QStringLiteral("Clear filter"), page);
    clear_filter_button_->setToolTip(QStringLiteral("Show all hosts again"));
    filter_row->addWidget(clear_filter_button_);
    add_expected_host_button_ = new QPushButton(QStringLiteral("Add Expected Host"), page);
    filter_row->addWidget(add_expected_host_button_);
    layout->addLayout(filter_row);

    main_splitter_ = new QSplitter(Qt::Horizontal, page);

    host_view_ = new QTableView(main_splitter_);
    host_view_->setModel(proxy_);
    host_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    host_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    host_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    host_view_->verticalHeader()->setVisible(false);
    host_view_->setContextMenuPolicy(Qt::CustomContextMenu);
    main_splitter_->addWidget(host_view_);

    auto* detail_panel = new QWidget(main_splitter_);
    auto* detail_layout = new QVBoxLayout(detail_panel);

    auto* header_layout = new QHBoxLayout();
    detail_hostname_label_ = new QLabel(QStringLiteral("No host selected"), detail_panel);
    QFont hostname_font = detail_hostname_label_->font();
    hostname_font.setBold(true);
    hostname_font.setPointSize(hostname_font.pointSize() + 2);
    detail_hostname_label_->setFont(hostname_font);
    detail_state_label_ = new QLabel(detail_panel);
    header_layout->addWidget(detail_hostname_label_);
    header_layout->addWidget(detail_state_label_);
    header_layout->addStretch();
    detail_layout->addLayout(header_layout);

    detail_layout->addWidget(new QLabel(QStringLiteral("CPU"), detail_panel));
    detail_cpu_sparkline_ = new lm::ui::Sparkline(detail_panel);
    detail_layout->addWidget(detail_cpu_sparkline_);

    detail_memory_bar_ = new lm::ui::MeterBar(detail_panel);
    detail_layout->addWidget(make_gauge_row(detail_panel, QStringLiteral("Memory"), detail_memory_bar_));

    detail_disk_layout_ = new QVBoxLayout();
    detail_layout->addLayout(detail_disk_layout_);

    detail_compliance_tree_ = new QTreeWidget(detail_panel);
    detail_compliance_tree_->setColumnCount(3);
    detail_compliance_tree_->setHeaderLabels(
        {QStringLiteral("Rule"), QStringLiteral("Status"), QStringLiteral("Observed")});
    detail_layout->addWidget(detail_compliance_tree_, 1);

    main_splitter_->addWidget(detail_panel);
    main_splitter_->setStretchFactor(0, 1);
    main_splitter_->setStretchFactor(1, 2);

    layout->addWidget(main_splitter_, 1);

    connect(host_view_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &FleetWindow::on_selection_changed);
    connect(filter_edit_, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(host_view_, &QWidget::customContextMenuRequested, this, &FleetWindow::on_context_menu_requested);
    connect(ribbon_, &StatusRibbon::filter_requested, this, &FleetWindow::on_filter_requested);
    connect(ribbon_, &StatusRibbon::stale_filter_requested, this, &FleetWindow::on_stale_filter_requested);
    connect(clear_filter_button_, &QPushButton::clicked, this, &FleetWindow::clear_filters);
    connect(filter_edit_, &QLineEdit::textChanged, this, [this](const QString&) { update_filter_status(); });
    // The model is re-applied every reconcile tick, so the "showing N of M"
    // count has to follow it rather than only changing when a filter changes.
    connect(proxy_, &QAbstractItemModel::layoutChanged, this, &FleetWindow::update_filter_status);
    connect(proxy_, &QAbstractItemModel::rowsInserted, this, &FleetWindow::update_filter_status);
    connect(proxy_, &QAbstractItemModel::rowsRemoved, this, &FleetWindow::update_filter_status);
    connect(add_expected_host_button_, &QPushButton::clicked, this, &FleetWindow::on_add_expected_host_clicked);

    tabs_->addTab(page, QStringLiteral("Fleet"));
}

void FleetWindow::build_templates_tab() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    auto* splitter = new QSplitter(Qt::Horizontal, page);

    auto* left = new QWidget(splitter);
    auto* left_layout = new QVBoxLayout(left);
    template_list_ = new QListWidget(left);
    left_layout->addWidget(template_list_, 1);
    auto* template_buttons = new QHBoxLayout();
    auto* add_template_button = new QPushButton(QStringLiteral("Add Template"), left);
    auto* remove_template_button = new QPushButton(QStringLiteral("Remove Template"), left);
    template_buttons->addWidget(add_template_button);
    template_buttons->addWidget(remove_template_button);
    left_layout->addLayout(template_buttons);
    splitter->addWidget(left);

    auto* right = new QWidget(splitter);
    auto* right_layout = new QVBoxLayout(right);

    right_layout->addWidget(new QLabel(QStringLiteral("Rules"), right));
    rule_table_ = new QTableWidget(0, 4, right);
    rule_table_->setHorizontalHeaderLabels(
        {QStringLiteral("ID"), QStringLiteral("Kind"), QStringLiteral("Expectation"), QStringLiteral("Target")});
    rule_table_->horizontalHeader()->setStretchLastSection(true);
    rule_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rule_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rule_table_->verticalHeader()->setVisible(false);
    right_layout->addWidget(rule_table_, 1);
    auto* rule_buttons = new QHBoxLayout();
    auto* add_rule_button = new QPushButton(QStringLiteral("Add Rule"), right);
    auto* remove_rule_button = new QPushButton(QStringLiteral("Remove Rule"), right);
    rule_buttons->addWidget(add_rule_button);
    rule_buttons->addWidget(remove_rule_button);
    right_layout->addLayout(rule_buttons);

    right_layout->addWidget(new QLabel(QStringLiteral("Host -> Template Assignments"), right));
    assignment_table_ = new QTableWidget(0, 2, right);
    assignment_table_->setHorizontalHeaderLabels(
        {QStringLiteral("Host"), QStringLiteral("Templates (comma-separated)")});
    assignment_table_->horizontalHeader()->setStretchLastSection(true);
    assignment_table_->verticalHeader()->setVisible(false);
    right_layout->addWidget(assignment_table_, 1);
    auto* assignment_buttons = new QHBoxLayout();
    auto* add_assignment_button = new QPushButton(QStringLiteral("Add Host"), right);
    auto* remove_assignment_button = new QPushButton(QStringLiteral("Remove Host"), right);
    assignment_buttons->addWidget(add_assignment_button);
    assignment_buttons->addWidget(remove_assignment_button);
    right_layout->addLayout(assignment_buttons);

    splitter->addWidget(right);
    layout->addWidget(splitter, 1);

    auto* publish_row = new QHBoxLayout();
    publish_status_label_ = new QLabel(QStringLiteral("Draft matches published bundle"), page);
    publish_button_ = new QPushButton(QStringLiteral("Publish"), page);
    publish_row->addWidget(publish_status_label_, 1);
    publish_row->addWidget(publish_button_);
    layout->addLayout(publish_row);

    connect(template_list_, &QListWidget::currentItemChanged, this, &FleetWindow::on_template_selection_changed);
    connect(add_template_button, &QPushButton::clicked, this, &FleetWindow::on_add_template_clicked);
    connect(remove_template_button, &QPushButton::clicked, this, &FleetWindow::on_remove_template_clicked);
    connect(add_rule_button, &QPushButton::clicked, this, &FleetWindow::on_add_rule_clicked);
    connect(remove_rule_button, &QPushButton::clicked, this, &FleetWindow::on_remove_rule_clicked);
    connect(assignment_table_, &QTableWidget::cellChanged, this, &FleetWindow::on_assignment_cell_changed);
    connect(add_assignment_button, &QPushButton::clicked, this, &FleetWindow::on_add_assignment_clicked);
    connect(remove_assignment_button, &QPushButton::clicked, this, &FleetWindow::on_remove_assignment_clicked);
    connect(publish_button_, &QPushButton::clicked, this, &FleetWindow::on_publish_clicked);

    rebuild_template_list();
    rebuild_rule_table();
    rebuild_assignment_table();

    tabs_->addTab(page, QStringLiteral("Templates"));
}

QString FleetWindow::selected_host_id() const {
    if (host_view_->selectionModel() == nullptr) {
        return {};
    }
    const QModelIndexList selected = host_view_->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        return {};
    }
    const QModelIndex source_index = proxy_->mapToSource(selected.first());
    return controller_->model()->data(source_index, lm::ui::FleetModel::HostIdRole).toString();
}

void FleetWindow::on_selection_changed() {
    const QString host_id = selected_host_id();
    if (host_id.isEmpty()) {
        detail_state_label_->clear();
    } else {
        const QModelIndexList selected = host_view_->selectionModel()->selectedRows();
        const QModelIndex source_index = proxy_->mapToSource(selected.first());
        const auto state =
            static_cast<lm::core::HostState>(controller_->model()->data(source_index, lm::ui::FleetModel::StateRole).toInt());
        detail_state_label_->setText(QString::fromStdString(lm::core::to_string(state)));
    }
    refresh_detail_pane(host_id);
}

void FleetWindow::refresh_detail_pane(const QString& host_id) {
    detail_hostname_label_->setText(host_id.isEmpty() ? QStringLiteral("No host selected") : host_id);
    detail_cpu_sparkline_->clear();
    detail_compliance_tree_->clear();

    if (host_id.isEmpty()) {
        sync_disk_bars({});
        detail_memory_bar_->set_value(0.0);
        return;
    }

    const auto& resources = controller_->resource_cache();
    const auto res_it = resources.constFind(host_id);
    if (res_it != resources.constEnd()) {
        detail_cpu_sparkline_->push(res_it->cpu_percent);
        const double mem_percent = res_it->mem_total_bytes == 0
                                        ? 0.0
                                        : 100.0 * static_cast<double>(res_it->mem_used_bytes) /
                                              static_cast<double>(res_it->mem_total_bytes);
        detail_memory_bar_->set_value(mem_percent);
        sync_disk_bars(res_it->disks);
    } else {
        detail_memory_bar_->set_value(0.0);
        sync_disk_bars({});
    }

    const auto& reports = controller_->report_cache();
    const auto rep_it = reports.constFind(host_id);
    if (rep_it != reports.constEnd()) {
        populate_compliance_tree(*rep_it);
    }
}

void FleetWindow::on_resource_sample(QString host_id, lm::core::ResourceSample sample) {
    if (host_id != selected_host_id()) {
        return;
    }
    detail_cpu_sparkline_->push(sample.cpu_percent);
    const double mem_percent = sample.mem_total_bytes == 0
                                    ? 0.0
                                    : 100.0 * static_cast<double>(sample.mem_used_bytes) /
                                          static_cast<double>(sample.mem_total_bytes);
    detail_memory_bar_->set_value(mem_percent);
    sync_disk_bars(sample.disks);
}

void FleetWindow::on_compliance_report(QString host_id, lm::core::ComplianceReport report) {
    if (host_id != selected_host_id()) {
        return;
    }
    detail_compliance_tree_->clear();
    populate_compliance_tree(report);
}

void FleetWindow::populate_compliance_tree(const lm::core::ComplianceReport& report) {
    // Grouped by CheckStatus rather than by RuleKind: CheckResult (the only
    // thing ComplianceReportMessage ever carries) has no RuleKind field, so
    // Applications/Services/Registry grouping is not derivable here -- same
    // limitation, and same resolution, as the client's DetailWindow.
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

        auto* header = new QTreeWidgetItem(detail_compliance_tree_);
        header->setText(0, QStringLiteral("%1 (%2)").arg(group.title).arg(matches.size()));
        header->setFirstColumnSpanned(true);

        for (const lm::core::CheckResult* result : matches) {
            auto* row = new QTreeWidgetItem(header);
            row->setText(0, QString::fromStdString(result->rule_id));
            row->setText(1, lm::ui::Theme::glyph_for(result->status));
            row->setText(2, QString::fromStdString(result->observed.empty() ? result->message : result->observed));
            const QColor color = result->status == lm::core::CheckStatus::NotApplicable
                                      ? QColor(lm::ui::Theme::kTextMuted)
                                      : lm::ui::Theme::color_for(result->status);
            for (int column = 0; column < 3; ++column) {
                row->setForeground(column, color);
            }
        }
        header->setExpanded(true);
    }
}

void FleetWindow::sync_disk_bars(const std::vector<lm::core::DiskUsage>& disks) {
    QSet<QString> seen;
    for (const lm::core::DiskUsage& disk : disks) {
        const QString mount = QString::fromStdString(disk.mount);
        seen.insert(mount);

        auto it = detail_disk_bars_.find(mount);
        lm::ui::MeterBar* bar = nullptr;
        if (it == detail_disk_bars_.end()) {
            bar = new lm::ui::MeterBar();
            detail_disk_layout_->addWidget(make_gauge_row(nullptr, mount, bar));
            detail_disk_bars_.insert(mount, bar);
        } else {
            bar = it.value();
        }
        bar->set_value(disk.used_percent());
    }

    for (auto it = detail_disk_bars_.begin(); it != detail_disk_bars_.end();) {
        if (seen.contains(it.key())) {
            ++it;
        } else {
            it.value()->parentWidget()->deleteLater();
            it = detail_disk_bars_.erase(it);
        }
    }
}

void FleetWindow::on_filter_requested(std::optional<lm::core::HostState> state) {
    state_filter_ = state;
    proxy_->set_state_filter(state);
    update_filter_status();
}

void FleetWindow::on_stale_filter_requested(bool active) {
    stale_filter_ = active;
    proxy_->set_stale_only(active);
    update_filter_status();
}

void FleetWindow::clear_filters() {
    state_filter_.reset();
    stale_filter_ = false;
    proxy_->set_state_filter(std::nullopt);
    proxy_->set_stale_only(false);
    filter_edit_->clear();
    ribbon_->clear_active();
    update_filter_status();
}

void FleetWindow::update_filter_status() {
    QStringList active;
    if (state_filter_) {
        active << QString::fromStdString(lm::core::to_string(*state_filter_));
    }
    if (stale_filter_) {
        active << QStringLiteral("Stale");
    }
    if (!filter_edit_->text().trimmed().isEmpty()) {
        active << QStringLiteral("name contains \"%1\"").arg(filter_edit_->text().trimmed());
    }

    const int shown = proxy_->rowCount();
    const int total = controller_->model()->rowCount();

    if (active.isEmpty()) {
        filter_status_label_->setText(total == 0 ? QString()
                                                 : QStringLiteral("%1 hosts").arg(total));
        clear_filter_button_->setVisible(false);
        return;
    }

    filter_status_label_->setText(
        QStringLiteral("Showing %1 of %2 - filtered by %3").arg(shown).arg(total).arg(active.join(QStringLiteral(" + "))));
    clear_filter_button_->setVisible(true);
}

void FleetWindow::on_context_menu_requested(const QPoint& pos) {
    const QModelIndex index = host_view_->indexAt(pos);
    if (!index.isValid()) {
        return;
    }
    const QModelIndex source_index = proxy_->mapToSource(index);
    const QString host_id = controller_->model()->data(source_index, lm::ui::FleetModel::HostIdRole).toString();
    const auto state =
        static_cast<lm::core::HostState>(controller_->model()->data(source_index, lm::ui::FleetModel::StateRole).toInt());

    QMenu menu(this);
    if (state == lm::core::HostState::Unexpected) {
        QAction* add_action = menu.addAction(QStringLiteral("Add to Expected Hosts"));
        connect(add_action, &QAction::triggered, this,
                [this, host_id] { controller_->add_expected_host(host_id.toStdString(), std::string{}); });
    } else {
        QAction* remove_action = menu.addAction(QStringLiteral("Remove from Expected Hosts"));
        connect(remove_action, &QAction::triggered, this,
                [this, host_id] { controller_->remove_expected_host(host_id.toStdString()); });
    }
    menu.exec(host_view_->viewport()->mapToGlobal(pos));
}

void FleetWindow::on_add_expected_host_clicked() {
    bool ok = false;
    const QString host_id = QInputDialog::getText(this, QStringLiteral("Add Expected Host"),
                                                    QStringLiteral("Hostname:"), QLineEdit::Normal, {}, &ok);
    if (!ok || host_id.trimmed().isEmpty()) {
        return;
    }

    // Address is optional -- ServerController::add_expected_host() treats an
    // empty string the same way the context menu's "Add to Expected Hosts"
    // action already does for an already-discovered host.
    const QString address = QInputDialog::getText(this, QStringLiteral("Add Expected Host"),
                                                    QStringLiteral("Address (optional):"), QLineEdit::Normal, {},
                                                    &ok);
    if (!ok) {
        return;
    }

    controller_->add_expected_host(host_id.trimmed().toStdString(), address.trimmed().toStdString());
}

void FleetWindow::on_publish_clicked() { controller_->publish(); }

void FleetWindow::on_draft_publishable_changed(bool can_publish) {
    publish_button_->setEnabled(can_publish);
    publish_status_label_->setText(can_publish ? QStringLiteral("Unpublished changes pending")
                                                : QStringLiteral("Draft matches published bundle"));
}

lm::core::Template* FleetWindow::selected_template() {
    QListWidgetItem* item = template_list_->currentItem();
    if (item == nullptr) {
        return nullptr;
    }
    const QString name = item->text();
    if (name == QStringLiteral("Baseline")) {
        return &controller_->draft().baseline;
    }
    for (lm::core::Template& tmpl : controller_->draft().templates) {
        if (QString::fromStdString(tmpl.name) == name) {
            return &tmpl;
        }
    }
    return nullptr;
}

void FleetWindow::rebuild_templates_view() {
    rebuild_template_list();
    rebuild_rule_table();
    rebuild_assignment_table();
}

void FleetWindow::rebuild_template_list() {
    const QString previous = template_list_->currentItem() != nullptr ? template_list_->currentItem()->text() : QString();
    template_list_->clear();
    template_list_->addItem(QStringLiteral("Baseline"));
    for (const lm::core::Template& tmpl : controller_->draft().templates) {
        template_list_->addItem(QString::fromStdString(tmpl.name));
    }
    const QList<QListWidgetItem*> matches = template_list_->findItems(previous, Qt::MatchExactly);
    if (!matches.isEmpty()) {
        template_list_->setCurrentItem(matches.first());
    } else {
        template_list_->setCurrentRow(0);
    }
}

void FleetWindow::rebuild_rule_table() {
    rule_table_->setRowCount(0);
    lm::core::Template* tmpl = selected_template();
    if (tmpl == nullptr) {
        return;
    }
    rule_table_->setRowCount(static_cast<int>(tmpl->rules.size()));
    for (int row = 0; row < rule_table_->rowCount(); ++row) {
        const lm::core::Rule& rule = tmpl->rules[static_cast<std::size_t>(row)];
        rule_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(rule.id)));
        rule_table_->setItem(row, 1, new QTableWidgetItem(kind_label(lm::core::kind_of(rule))));
        rule_table_->setItem(row, 2,
                              new QTableWidgetItem(rule.expectation == lm::core::Presence::MustBePresent
                                                        ? QStringLiteral("Must be present")
                                                        : QStringLiteral("Must be absent")));
        rule_table_->setItem(row, 3, new QTableWidgetItem(target_label(rule)));
    }
}

void FleetWindow::rebuild_assignment_table() {
    updating_assignment_table_ = true;
    assignment_table_->setRowCount(0);
    const auto& assignments = controller_->draft().assignments;
    assignment_table_->setRowCount(static_cast<int>(assignments.size()));
    int row = 0;
    for (const auto& [host_id, templates] : assignments) {
        assignment_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(host_id)));
        QStringList names;
        for (const std::string& name : templates) {
            names << QString::fromStdString(name);
        }
        assignment_table_->setItem(row, 1, new QTableWidgetItem(names.join(QStringLiteral(", "))));
        ++row;
    }
    updating_assignment_table_ = false;
}

void FleetWindow::on_template_selection_changed() { rebuild_rule_table(); }

void FleetWindow::on_add_template_clicked() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("New Template"), QStringLiteral("Template name:"),
                               QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    lm::core::Template tmpl;
    tmpl.name = name.trimmed().toStdString();
    controller_->draft().templates.push_back(std::move(tmpl));
    controller_->mark_draft_dirty();
    rebuild_template_list();
}

void FleetWindow::on_remove_template_clicked() {
    QListWidgetItem* item = template_list_->currentItem();
    if (item == nullptr || item->text() == QStringLiteral("Baseline")) {
        return;
    }
    const QString name = item->text();
    auto& templates = controller_->draft().templates;
    templates.erase(std::remove_if(templates.begin(), templates.end(),
                                    [&](const lm::core::Template& tmpl) {
                                        return QString::fromStdString(tmpl.name) == name;
                                    }),
                     templates.end());
    controller_->mark_draft_dirty();
    rebuild_template_list();
    rebuild_rule_table();
}

void FleetWindow::on_add_rule_clicked() {
    lm::core::Template* tmpl = selected_template();
    if (tmpl == nullptr) {
        QMessageBox::information(this, QStringLiteral("Add Rule"), QStringLiteral("Select a template first."));
        return;
    }

    bool ok = false;
    const QString id = QInputDialog::getText(this, QStringLiteral("Rule ID"), QStringLiteral("Unique rule id:"),
                                              QLineEdit::Normal, {}, &ok);
    if (!ok || id.trimmed().isEmpty()) {
        return;
    }

    const QStringList kinds{QStringLiteral("Process"), QStringLiteral("Service"), QStringLiteral("Registry")};
    const QString kind =
        QInputDialog::getItem(this, QStringLiteral("Rule Kind"), QStringLiteral("Kind:"), kinds, 0, false, &ok);
    if (!ok) {
        return;
    }

    const QStringList expectations{QStringLiteral("Must be present"), QStringLiteral("Must be absent")};
    const QString expectation_text = QInputDialog::getItem(
        this, QStringLiteral("Expectation"), QStringLiteral("Expectation:"), expectations, 0, false, &ok);
    if (!ok) {
        return;
    }

    const QString description = QInputDialog::getText(
        this, QStringLiteral("Description"), QStringLiteral("Description (optional):"), QLineEdit::Normal, {}, &ok);
    if (!ok) {
        return;
    }

    lm::core::Rule rule;
    rule.id = id.trimmed().toStdString();
    rule.description = description.trimmed().toStdString();
    rule.expectation = expectation_text == QStringLiteral("Must be present") ? lm::core::Presence::MustBePresent
                                                                              : lm::core::Presence::MustBeAbsent;

    if (kind == QStringLiteral("Process")) {
        const QString exe = QInputDialog::getText(this, QStringLiteral("Executable"),
                                                    QStringLiteral("Executable name:"), QLineEdit::Normal, {}, &ok);
        if (!ok || exe.trimmed().isEmpty()) {
            return;
        }
        rule.payload = lm::core::ProcessRule{exe.trimmed().toStdString()};
    } else if (kind == QStringLiteral("Service")) {
        const QString service = QInputDialog::getText(this, QStringLiteral("Service"),
                                                        QStringLiteral("Service name:"), QLineEdit::Normal, {}, &ok);
        if (!ok || service.trimmed().isEmpty()) {
            return;
        }
        rule.payload = lm::core::ServiceRule{service.trimmed().toStdString(), std::nullopt};
    } else {
        const QStringList hives{QStringLiteral("HKLM"), QStringLiteral("HKCU"), QStringLiteral("HKCR"),
                                 QStringLiteral("HKU")};
        const QString hive_text = QInputDialog::getItem(this, QStringLiteral("Registry Hive"),
                                                          QStringLiteral("Hive:"), hives, 0, false, &ok);
        if (!ok) {
            return;
        }
        const QString key_path = QInputDialog::getText(this, QStringLiteral("Registry Key"),
                                                         QStringLiteral("Key path:"), QLineEdit::Normal, {}, &ok);
        if (!ok) {
            return;
        }
        const QString value_name = QInputDialog::getText(
            this, QStringLiteral("Registry Value"), QStringLiteral("Value name:"), QLineEdit::Normal, {}, &ok);
        if (!ok) {
            return;
        }
        const QStringList matches{QStringLiteral("Exists"), QStringLiteral("Equals"), QStringLiteral("Contains")};
        const QString match_text =
            QInputDialog::getItem(this, QStringLiteral("Match"), QStringLiteral("Match:"), matches, 0, false, &ok);
        if (!ok) {
            return;
        }
        QString expected_value;
        if (match_text != QStringLiteral("Exists")) {
            expected_value = QInputDialog::getText(this, QStringLiteral("Expected Value"),
                                                     QStringLiteral("Expected value:"), QLineEdit::Normal, {}, &ok);
            if (!ok) {
                return;
            }
        }

        lm::core::RegistryRule reg;
        reg.hive = lm::core::parse_registry_hive(hive_text.toStdString()).value_or(lm::core::RegistryHive::LocalMachine);
        reg.key_path = key_path.trimmed().toStdString();
        reg.value_name = value_name.trimmed().toStdString();
        reg.match = match_text == QStringLiteral("Equals")     ? lm::core::RegistryMatch::Equals
                    : match_text == QStringLiteral("Contains") ? lm::core::RegistryMatch::Contains
                                                                : lm::core::RegistryMatch::Exists;
        reg.expected_value = expected_value.toStdString();
        rule.payload = reg;
    }

    tmpl->rules.push_back(std::move(rule));
    controller_->mark_draft_dirty();
    rebuild_rule_table();
}

void FleetWindow::on_remove_rule_clicked() {
    lm::core::Template* tmpl = selected_template();
    if (tmpl == nullptr) {
        return;
    }
    const int row = rule_table_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= tmpl->rules.size()) {
        return;
    }
    tmpl->rules.erase(tmpl->rules.begin() + row);
    controller_->mark_draft_dirty();
    rebuild_rule_table();
}

void FleetWindow::on_add_assignment_clicked() {
    bool ok = false;
    const QString host_id = QInputDialog::getText(this, QStringLiteral("Add Assignment"),
                                                    QStringLiteral("Host id:"), QLineEdit::Normal, {}, &ok);
    if (!ok || host_id.trimmed().isEmpty()) {
        return;
    }
    // operator[] default-inserts an empty template list if absent, which is
    // exactly what "add this host with no templates yet" should do.
    (void)controller_->draft().assignments[host_id.trimmed().toStdString()];
    controller_->mark_draft_dirty();
    rebuild_assignment_table();
}

void FleetWindow::on_remove_assignment_clicked() {
    const int row = assignment_table_->currentRow();
    if (row < 0) {
        return;
    }
    QTableWidgetItem* host_item = assignment_table_->item(row, 0);
    if (host_item == nullptr) {
        return;
    }
    controller_->draft().assignments.erase(host_item->text().toStdString());
    controller_->mark_draft_dirty();
    rebuild_assignment_table();
}

void FleetWindow::on_assignment_cell_changed(int row, int /*column*/) {
    if (updating_assignment_table_) {
        return;
    }
    QTableWidgetItem* host_item = assignment_table_->item(row, 0);
    QTableWidgetItem* templates_item = assignment_table_->item(row, 1);
    if (host_item == nullptr || templates_item == nullptr) {
        return;
    }

    const std::string host_id = host_item->text().trimmed().toStdString();
    if (host_id.empty()) {
        return;
    }

    std::vector<std::string> names;
    const QStringList parts = templates_item->text().split(QChar(u','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) {
            names.push_back(trimmed.toStdString());
        }
    }

    controller_->draft().assignments[host_id] = std::move(names);
    controller_->mark_draft_dirty();
}

void FleetWindow::save_window_state() const {
    QSettings settings;
    settings.beginGroup(QStringLiteral("FleetWindow"));
    settings.setValue(QStringLiteral("geometry"), saveGeometry());
    settings.setValue(QStringLiteral("splitter"), main_splitter_->saveState());
    settings.endGroup();
}

void FleetWindow::restore_window_state() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("FleetWindow"));
    if (settings.contains(QStringLiteral("geometry"))) {
        restoreGeometry(settings.value(QStringLiteral("geometry")).toByteArray());
    }
    if (settings.contains(QStringLiteral("splitter"))) {
        main_splitter_->restoreState(settings.value(QStringLiteral("splitter")).toByteArray());
    }
    settings.endGroup();
}
