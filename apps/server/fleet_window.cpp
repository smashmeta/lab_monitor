#include "fleet_window.hpp"

#include <QAction>
#include <QCloseEvent>
#include <QDateTime>
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

#include <spdlog/spdlog.h>
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
#include <QTimer>
#include <QTimeZone>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "lm/core/rule.hpp"
#include "lm/ui/adapter_list.hpp"
#include "lm/ui/fleet_model.hpp"
#include "lm/ui/compliance_tag_delegate.hpp"
#include "lm/ui/keep_foreground_delegate.hpp"
#include "lm/ui/log_view.hpp"
#include "lm/ui/rule_detail.hpp"
#include "lm/ui/meter_bar.hpp"
#include "lm/ui/sparkline.hpp"
#include "lm/ui/theme.hpp"
#include "lm/ui/token_edit.hpp"
#include "add_rule_dialog.hpp"
#include "scripts_tab.hpp"
#include "server_controller.hpp"
#include "status_ribbon.hpp"

namespace {

/// Template list rows are identified by these, never by their label: the
/// baseline row's label carries an explanatory suffix, and a template
/// genuinely *named* "Baseline" must stay distinguishable from the bundle's
/// own baseline — otherwise it is neither selectable nor removable.
constexpr int kTemplateNameRole = Qt::UserRole;
constexpr int kIsBaselineRole = Qt::UserRole + 1;

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
        case lm::core::RuleKind::Network:  return QStringLiteral("Network");
        case lm::core::RuleKind::Dds:      return QStringLiteral("DDS");
    }
    return QStringLiteral("Unknown");
}

QString status_name(lm::core::CheckStatus status) {
    switch (status) {
        case lm::core::CheckStatus::Pass:          return QStringLiteral("Pass");
        case lm::core::CheckStatus::Fail:          return QStringLiteral("Fail");
        case lm::core::CheckStatus::Error:         return QStringLiteral("Error");
        case lm::core::CheckStatus::NotApplicable: return QStringLiteral("Not applicable");
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
            } else if constexpr (std::is_same_v<T, lm::core::RegistryRule>) {
                return QString::fromStdString(lm::core::registry_key(payload));
            } else if constexpr (std::is_same_v<T, lm::core::AdapterCountRule>) {
                return QStringLiteral("%1 %2 adapters connected")
                    .arg(QString::fromStdString(lm::core::to_string(payload.comparison)))
                    .arg(payload.count);
            } else if constexpr (std::is_same_v<T, lm::core::AdapterStateRule>) {
                return QStringLiteral("%1 link %2")
                    .arg(QString::fromStdString(payload.adapter_name),
                          QString::fromStdString(lm::core::to_string(payload.expected)));
            } else if constexpr (std::is_same_v<T, lm::core::DdsTopicRule>) {
                return QStringLiteral("%1 on domain %2")
                    .arg(QString::fromStdString(payload.topic_name))
                    .arg(payload.domain_id);
            } else {
                return QStringLiteral("%1.%2 on domain %3 %4 %5")
                    .arg(QString::fromStdString(payload.topic_name),
                          QString::fromStdString(payload.path))
                    .arg(payload.domain_id)
                    .arg(QString::fromStdString(lm::core::to_string(payload.match)),
                          QString::fromStdString(payload.expected_value));
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

    // beginFilterChange() before the state is touched, endFilterChange()
    // after, rather than Qt 5's invalidateFilter() once the change had already
    // happened -- that is the point of the pair, and it is why
    // invalidateFilter() is deprecated. Direction::Rows because this filter
    // only ever rejects rows (filterAcceptsRow below); the default is Both,
    // which would have the model re-examine every column for nothing.
    void set_state_filter(std::optional<lm::core::HostState> state) {
        beginFilterChange();
        state_filter_ = state;
        endFilterChange(Direction::Rows);
    }

    void set_stale_only(bool stale_only) {
        beginFilterChange();
        stale_only_ = stale_only;
        endFilterChange(Direction::Rows);
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
    build_scripts_tab();
    // Last, so the Log tab sits to the right of the three tabs somebody opens
    // this window to use.
    build_log_tab();

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

    // Trim the chrome above the table: every pixel here is a row lost.
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

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

    // Density target: ~35 hosts visible without scrolling in a maximised window
    // at 1920x1080. After the title bar, tab bar, ribbon, filter row and column
    // header, roughly 800px of body is left, so rows must stay near 22px.
    // Fixed rather than ResizeToContents so one long hostname cannot silently
    // inflate every row and cost a third of the list.
    host_view_->verticalHeader()->setDefaultSectionSize(22);
    host_view_->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    host_view_->setShowGrid(false);
    host_view_->setAlternatingRowColors(true);
    host_view_->setWordWrap(false);
    host_view_->horizontalHeader()->setFixedHeight(24);

    // Without this a selected row is painted with QPalette::HighlightedText,
    // discarding the health colour exactly when the operator has clicked the
    // row they care about. The delegate keeps the text colour and lets the
    // selection show through the background instead.
    host_view_->setItemDelegate(new lm::ui::KeepForegroundDelegate(host_view_));
    // Per column, so KeepForegroundDelegate still owns every other cell. This
    // one draws the ratio and then a tag per rule the host is failing or could
    // not check -- the fleet table is where compliance is read now, so a host
    // in trouble says what it is in trouble about on its own row.
    host_view_->setItemDelegateForColumn(lm::ui::FleetModel::ComplianceColumn,
                                         new lm::ui::ComplianceTagDelegate(host_view_));
    // Every other column holds a short fixed reading -- a state word, a
    // percentage, a timestamp -- so each is sized to its own content and the
    // compliance column absorbs everything left over. Without this they all sit
    // at the default section width, the tags get whatever is left, and a host
    // failing four rules renders as "1 / 5  +4 more" with not one of them named
    // -- which is the entire point of the column, behind a tooltip.
    for (int column = 0; column < lm::ui::FleetModel::ColumnCount; ++column) {
        host_view_->horizontalHeader()->setSectionResizeMode(
            column, column == lm::ui::FleetModel::ComplianceColumn
                        ? QHeaderView::Stretch
                        : QHeaderView::ResizeToContents);
    }
    // Hidden rather than removed from the model: both are still real readings,
    // the stale flag still drives the ribbon's Stale counter and its filter,
    // and the Host cell's tooltip carries both -- so nothing is lost, it is
    // just no longer taking width from the row every second of every day.
    host_view_->setColumnHidden(lm::ui::FleetModel::RevisionColumn, true);
    host_view_->setColumnHidden(lm::ui::FleetModel::LastSeenColumn, true);
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
    // Green when idle through to red when saturated, matching the client's
    // pane. The chart's vertical axis auto-scales to the window it is showing,
    // so height alone says nothing about the absolute level.
    detail_cpu_sparkline_->set_color_ramp(&lm::ui::Theme::color_for_load);
    detail_layout->addWidget(detail_cpu_sparkline_);

    detail_memory_bar_ = new lm::ui::MeterBar(detail_panel);
    detail_layout->addWidget(make_gauge_row(detail_panel, QStringLiteral("Memory"), detail_memory_bar_));

    detail_disk_layout_ = new QVBoxLayout();
    detail_layout->addLayout(detail_disk_layout_);

    detail_layout->addWidget(new QLabel(QStringLiteral("Network adapters"), detail_panel));
    detail_adapter_list_ = new lm::ui::AdapterList(detail_panel);
    detail_layout->addWidget(detail_adapter_list_, 1);

    detail_compliance_tree_ = new QTreeWidget(detail_panel);
    detail_compliance_tree_->setItemDelegate(
        new lm::ui::KeepForegroundDelegate(detail_compliance_tree_));
    detail_compliance_tree_->setColumnCount(3);
    detail_compliance_tree_->setHeaderLabels(
        {QStringLiteral("Rule"), QStringLiteral("Status"), QStringLiteral("Observed")});
    detail_layout->addWidget(detail_compliance_tree_, 1);

    main_splitter_->addWidget(detail_panel);
    // The table gets the larger share, not the detail pane. It did not used to:
    // every column was a short reading and the pane below carried the depth.
    // The compliance column changed that -- the rules a host is failing are
    // named on its own row now, and a table too narrow to reach that column
    // puts the whole point of it behind a horizontal scrollbar.
    main_splitter_->setStretchFactor(0, 2);
    main_splitter_->setStretchFactor(1, 1);

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

void FleetWindow::set_log_file_path(const QString& path) { log_view_->set_log_file_path(path); }

void FleetWindow::build_scripts_tab() {
    scripts_tab_ = new ScriptsTab(controller_);
    tabs_->addTab(scripts_tab_, QStringLiteral("Scripts"));
}

void FleetWindow::build_log_tab() {
    log_view_ = new lm::ui::LogView();
    // Attached here rather than in main(): the view replays the ring buffer as
    // it attaches, so it has to exist first, and this is where it comes into
    // being. LogView removes its own sink in its destructor, which is what
    // keeps the sink from outliving the widget when this window is deleted at
    // shutdown -- both apps log after that point.
    log_view_->attach_to_default_logger();
    tabs_->addTab(log_view_, QStringLiteral("Log"));
}

void FleetWindow::build_templates_tab() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    auto* splitter = new QSplitter(Qt::Horizontal, page);

    auto* left = new QWidget(splitter);
    auto* left_layout = new QVBoxLayout(left);
    template_list_ = new QListWidget(left);
    // Named because the window now holds more than one QListWidget: the
    // Scripts tab has the host list, so "the only list in the window" stopped
    // being a way to find this one.
    template_list_->setObjectName(QStringLiteral("TemplateList"));
    left_layout->addWidget(template_list_, 1);
    auto* template_buttons = new QHBoxLayout();
    auto* add_template_button = new QPushButton(QStringLiteral("Add Template"), left);
    remove_template_button_ = new QPushButton(QStringLiteral("Remove Template"), left);
    template_buttons->addWidget(add_template_button);
    template_buttons->addWidget(remove_template_button_);
    left_layout->addLayout(template_buttons);
    splitter->addWidget(left);

    auto* right = new QWidget(splitter);
    auto* right_layout = new QVBoxLayout(right);

    right_layout->addWidget(new QLabel(QStringLiteral("Rules"), right));
    rule_table_ = new QTableWidget(0, 4, right);
    // The description, not the id. A rule id is a generated join key
    // ("process-chrome-exe", "registry-displayversion-2") that make_rule_id()
    // derives and nobody types; the description is the sentence the author
    // wrote the rule to say. The id is still one hover away in the row
    // tooltip, which is where a support conversation needs it.
    rule_table_->setHorizontalHeaderLabels({QStringLiteral("Description"), QStringLiteral("Kind"),
                                            QStringLiteral("Expectation"), QStringLiteral("Target")});
    rule_table_->horizontalHeader()->setStretchLastSection(true);
    rule_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    rule_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rule_table_->verticalHeader()->setVisible(false);
    right_layout->addWidget(rule_table_, 1);
    auto* rule_buttons = new QHBoxLayout();
    auto* add_rule_button = new QPushButton(QStringLiteral("Add Rule"), right);
    auto* edit_rule_button = new QPushButton(QStringLiteral("Edit Rule"), right);
    auto* remove_rule_button = new QPushButton(QStringLiteral("Remove Rule"), right);
    edit_rule_button->setObjectName(QStringLiteral("EditRuleButton"));
    rule_buttons->addWidget(add_rule_button);
    rule_buttons->addWidget(edit_rule_button);
    rule_buttons->addWidget(remove_rule_button);
    right_layout->addLayout(rule_buttons);

    right_layout->addWidget(new QLabel(QStringLiteral("Host -> Template Assignments"), right));
    assignment_table_ = new QTableWidget(0, 2, right);
    assignment_table_->setHorizontalHeaderLabels({QStringLiteral("Host"), QStringLiteral("Templates")});
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
    connect(remove_template_button_, &QPushButton::clicked, this, &FleetWindow::on_remove_template_clicked);
    connect(add_rule_button, &QPushButton::clicked, this, &FleetWindow::on_add_rule_clicked);
    connect(edit_rule_button, &QPushButton::clicked, this, &FleetWindow::on_edit_rule_clicked);
    connect(remove_rule_button, &QPushButton::clicked, this, &FleetWindow::on_remove_rule_clicked);
    // The button is what makes editing discoverable; the double-click is what
    // anyone tries first on a table. Both land on the same slot rather than on
    // two paths that could drift apart.
    connect(rule_table_, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem*) { on_edit_rule_clicked(); });
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
        detail_adapter_list_->clear();
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
    refresh_adapter_list(res_it != resources.constEnd() ? &*res_it : nullptr);

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
    refresh_adapter_list(&sample);
}

void FleetWindow::refresh_adapter_list(const lm::core::ResourceSample* sample) {
    // "Not reported" and "none" are different answers, and only the host's
    // advertised capabilities can tell them apart -- an empty adapter list from
    // a client that cannot enumerate them says nothing about the machine.
    const QModelIndexList selected = host_view_->selectionModel()->selectedRows();
    bool reports_adapters = false;
    if (!selected.isEmpty()) {
        const QModelIndex source = proxy_->mapToSource(selected.first());
        const auto caps = static_cast<std::uint32_t>(
            controller_->model()->data(source, lm::ui::FleetModel::CapabilitiesRole).toUInt());
        reports_adapters = lm::core::Capabilities(caps).has(lm::core::Capability::Network);
    }

    if (!reports_adapters) {
        detail_adapter_list_->set_not_reported();
        return;
    }
    detail_adapter_list_->set_adapters(sample != nullptr ? sample->adapters
                                                          : std::vector<lm::core::NetworkAdapter>{});
}

void FleetWindow::on_compliance_report(QString host_id, lm::core::ComplianceReport report) {
    // The fleet row itself is updated by ServerController, which owns the model
    // and the published bundle the rule descriptions come from. Only the detail
    // pane is this window's business, and only for the selected host.
    if (host_id != selected_host_id()) {
        return;
    }
    detail_compliance_tree_->clear();
    populate_compliance_tree(report);
}

void FleetWindow::populate_compliance_tree(const lm::core::ComplianceReport& report) {
    // CheckResult carries only a rule id, so recover each rule's description
    // and payload from the published bundle -- which this server owns, being
    // the thing that published it.
    //
    // rules_for(), rather than every template in the bundle: this host's rules
    // are the baseline plus the templates assigned to it, which is exactly the
    // set the client evaluated. Walking all templates instead meant a rule id
    // reused in an *unrelated* template could win the lookup and label the row
    // with a different rule's description -- and it resolved collisions the
    // opposite way round from rules_for() (last wins, not first), so the two
    // ends disagreed about the same id. Ids are unique now, but agreeing with
    // the client by construction beats relying on that.
    QHash<QString, lm::ui::RuleDetail> by_id;
    for (const lm::core::Rule* rule : lm::core::rules_for(controller_->published(), report.host_id)) {
        by_id.insert(QString::fromStdString(rule->id), lm::ui::describe(*rule));
    }

    // Grouped by CheckStatus rather than by RuleKind. `by_id` now carries each
    // rule's kind, so regrouping is a small change -- but status-first is what
    // an operator needs first, matching the client.
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

            const QString id = QString::fromStdString(result->rule_id);
            const auto detail = by_id.constFind(id);
            const bool described = detail != by_id.constEnd();

            // Show the authored description; fall back to the id only when the
            // result has no matching rule, meaning the bundle changed between
            // the client evaluating and this render.
            row->setText(0, described ? detail->label : id);
            row->setText(1, lm::ui::Theme::glyph_for(result->status));

            // Both fields are populated for an error, and observed alone is
            // often too terse to act on.
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

    // Rule descriptions are sentences, and the default column width truncated
    // them to "Wire…" — which defeats the point of showing the description
    // rather than the id. Capped so one long rule cannot crowd out Observed,
    // which carries the reason a check failed.
    detail_compliance_tree_->resizeColumnToContents(0);
    constexpr int kMaxRuleWidth = 320;
    detail_compliance_tree_->setColumnWidth(
        0, std::min(detail_compliance_tree_->columnWidth(0), kMaxRuleWidth));
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
    // By role, never by the row's label: the label carries an explanatory
    // suffix, and a stray template genuinely *named* "Baseline" (which older
    // builds let an assignment create) would otherwise be indistinguishable
    // from the bundle's own baseline and impossible to select or delete.
    if (item->data(kIsBaselineRole).toBool()) {
        return &controller_->draft().baseline;
    }
    const QString name = item->data(kTemplateNameRole).toString();
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
    const QListWidgetItem* current = template_list_->currentItem();
    const QString previous = current != nullptr ? current->data(kTemplateNameRole).toString() : QString();
    const bool previous_was_baseline = current != nullptr && current->data(kIsBaselineRole).toBool();
    template_list_->clear();

    // The baseline is a field of the bundle, not one of its templates: it is
    // applied to every host, it cannot be assigned and it cannot be removed.
    // Saying so in the row beats letting the operator discover it by clicking
    // Remove and watching nothing happen.
    auto* baseline_item = new QListWidgetItem(QStringLiteral("Baseline — always applied"));
    baseline_item->setData(kTemplateNameRole, QString::fromUtf8(lm::core::kBaselineName.data(),
                                                                 static_cast<int>(lm::core::kBaselineName.size())));
    baseline_item->setData(kIsBaselineRole, true);
    baseline_item->setToolTip(
        QStringLiteral("Applied to every host, expected or not. It is part of the bundle rather than\n"
                        "one of its templates, so it cannot be assigned, renamed or removed."));
    // Italic rather than a colour: it survives selection without needing the
    // foreground-preserving delegate the fleet table uses.
    QFont baseline_font = baseline_item->font();
    baseline_font.setItalic(true);
    baseline_item->setFont(baseline_font);
    template_list_->addItem(baseline_item);

    for (const lm::core::Template& tmpl : controller_->draft().templates) {
        const QString name = QString::fromStdString(tmpl.name);
        auto* item = new QListWidgetItem(name);
        item->setData(kTemplateNameRole, name);
        item->setData(kIsBaselineRole, false);
        template_list_->addItem(item);
    }

    // Restore by role, and only onto a row of the same kind, so a stray
    // template named "Baseline" cannot capture the baseline's own selection.
    for (int row = 0; row < template_list_->count(); ++row) {
        QListWidgetItem* item = template_list_->item(row);
        if (item->data(kIsBaselineRole).toBool() == previous_was_baseline &&
            item->data(kTemplateNameRole).toString() == previous) {
            template_list_->setCurrentItem(item);
            return;
        }
    }
    template_list_->setCurrentRow(0);
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
        // Through describe(), not from rule.expectation directly. Two kinds
        // carry their own direction -- the adapter count in its comparison, the
        // DDS value in its match -- and Add Rule does not ask them for a
        // presence at all. Printing the default "Must be present" beside
        // "at least 2" put a second, contradictable answer in the row for a
        // question already settled; describe() shows the constraint instead.
        const lm::ui::RuleDetail detail = lm::ui::describe(rule);
        // detail.label rather than rule.description: describe() substitutes
        // "Kind: target" when the author left the description blank, so the
        // column cannot show an empty cell for a rule that is really there.
        rule_table_->setItem(row, 0, new QTableWidgetItem(detail.label));
        rule_table_->setItem(row, 1, new QTableWidgetItem(kind_label(lm::core::kind_of(rule))));
        rule_table_->setItem(row, 2, new QTableWidgetItem(detail.expectation));
        rule_table_->setItem(row, 3, new QTableWidgetItem(target_label(rule)));

        // The Target column is the stretch column and a DDS path plus a domain
        // still outruns it on a narrow window, so the whole rule is a hover
        // away. A tooltip is right *here* and wrong on the Compliance tab: this
        // is an editing surface someone is sitting at, that one is read from
        // across a room where nothing may hide behind a gesture.
        for (int column = 0; column < rule_table_->columnCount(); ++column) {
            rule_table_->item(row, column)->setToolTip(detail.tooltip());
        }
    }
    // The Target column is the one that says what the rule actually checks, and
    // a DDS path plus a domain outgrows the default width immediately.
    rule_table_->resizeColumnsToContents();
    // A description is free text and resizeColumnsToContents has no ceiling, so
    // one written as a sentence would size column 0 to the sentence and push
    // Kind, Expectation and Target past the right edge. Cap it and let the cell
    // elide; the untruncated text is already in the tooltip.
    constexpr int kMaxDescriptionWidth = 280;
    if (rule_table_->columnWidth(0) > kMaxDescriptionWidth) {
        rule_table_->setColumnWidth(0, kMaxDescriptionWidth);
    }
}

void FleetWindow::rebuild_assignment_table() {
    updating_assignment_table_ = true;
    assignment_table_->setRowCount(0);
    const auto& assignments = controller_->draft().assignments;
    assignment_table_->setRowCount(static_cast<int>(assignments.size()));
    const QStringList known = template_names();

    int row = 0;
    for (const auto& [host_id, templates] : assignments) {
        const QString host = QString::fromStdString(host_id);

        auto* host_item = new QTableWidgetItem(host);
        // The id as it stands before any edit, so a rename can move the entry
        // instead of leaving the old one behind under its old key.
        host_item->setData(Qt::UserRole, host);
        assignment_table_->setItem(row, 0, host_item);

        QStringList names;
        for (const std::string& name : templates) {
            names << QString::fromStdString(name);
        }

        // A live widget in the cell rather than an editor opened on demand:
        // the chips and their remove buttons are the point, and they are no
        // use hidden behind a double-click.
        auto* editor = new lm::ui::TokenEdit(assignment_table_);
        editor->set_known_values(known);
        editor->set_tokens(names);
        connect(editor, &lm::ui::TokenEdit::tokens_changed, this,
                [this, host, editor](const QStringList& tokens) {
                    on_assignment_tokens_changed(host, editor, tokens);
                });
        assignment_table_->setCellWidget(row, 1, editor);
        ++row;
    }

    assignment_table_->resizeRowsToContents();
    updating_assignment_table_ = false;
}

QStringList FleetWindow::template_names() const {
    QStringList names;
    names.reserve(static_cast<int>(controller_->draft().templates.size()));
    for (const lm::core::Template& tmpl : controller_->draft().templates) {
        names << QString::fromStdString(tmpl.name);
    }
    return names;
}

void FleetWindow::refresh_assignment_completions() {
    const QStringList known = template_names();
    for (int row = 0; row < assignment_table_->rowCount(); ++row) {
        if (auto* editor = qobject_cast<lm::ui::TokenEdit*>(assignment_table_->cellWidget(row, 1))) {
            editor->set_known_values(known);
        }
    }
}

void FleetWindow::on_template_selection_changed() {
    const QListWidgetItem* item = template_list_->currentItem();
    // Greyed out rather than silently doing nothing when the baseline is
    // selected, which is what it did before.
    remove_template_button_->setEnabled(item != nullptr && !item->data(kIsBaselineRole).toBool());
    rebuild_rule_table();
}

void FleetWindow::on_add_template_clicked() {
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, QStringLiteral("New Template"), QStringLiteral("Template name:"),
                               QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }
    if (lm::core::is_baseline_name(name.trimmed().toStdString())) {
        QMessageBox::information(
            this, QStringLiteral("New Template"),
            QStringLiteral("\"Baseline\" is the bundle's own baseline, applied to every host.\n"
                            "Add rules to it by selecting it in the list; a second template of that\n"
                            "name would only shadow it."));
        return;
    }
    lm::core::Template tmpl;
    tmpl.name = name.trimmed().toStdString();
    spdlog::info("template added: '{}'", tmpl.name);
    controller_->draft().templates.push_back(std::move(tmpl));
    controller_->mark_draft_dirty();
    rebuild_template_list();
    // The assignment editors complete against this list, so it has to follow.
    refresh_assignment_completions();
}

void FleetWindow::on_remove_template_clicked() {
    QListWidgetItem* item = template_list_->currentItem();
    // The flag, not the label: a stray template named "Baseline" left over
    // from an older build has to be removable, and this is the only route to
    // getting rid of it.
    if (item == nullptr || item->data(kIsBaselineRole).toBool()) {
        return;
    }
    const QString name = item->data(kTemplateNameRole).toString();
    auto& templates = controller_->draft().templates;
    std::erase_if(templates, [&](const lm::core::Template& tmpl) {
        return QString::fromStdString(tmpl.name) == name;
    });
    spdlog::info("template removed: '{}'", name.toStdString());
    controller_->mark_draft_dirty();
    rebuild_template_list();
    rebuild_rule_table();
    // Assignments naming it are left alone: rules_for() already ignores a name
    // with no template behind it, and silently editing the operator's
    // assignments because they deleted something is worse than a stale chip.
    refresh_assignment_completions();
}

void FleetWindow::on_add_rule_clicked() {
    lm::core::Template* tmpl = selected_template();
    if (tmpl == nullptr) {
        QMessageBox::information(this, QStringLiteral("Add Rule"),
                                  QStringLiteral("Select a template first."));
        return;
    }

    // One dialog rather than the chain of seven prompts this used to be: the
    // fields for the chosen kind are visible together, and the summary line
    // says what the rule will check before it is created. See
    // add_rule_dialog.hpp for the mis-authored DDS rule that prompted it.
    AddRuleDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    lm::core::Rule rule = dialog.rule();
    // The id is generated here, not in the dialog: it must be unique across the
    // whole draft, since templates are combined per host and an id taken in any
    // of them is taken here too. The dialog cannot see the draft.
    rule.id = lm::core::make_rule_id(controller_->draft(), rule);

    // Logged with what it checks rather than its id: the id is generated and
    // says nothing, and the draft is not published yet, so this is the only
    // record of the edit until somebody presses Publish.
    spdlog::info("rule added to template '{}': {} [{}]", tmpl->name,
                 lm::ui::describe(rule).label.toStdString(), rule.id);
    tmpl->rules.push_back(std::move(rule));
    controller_->mark_draft_dirty();
    rebuild_rule_table();
}

void FleetWindow::on_edit_rule_clicked() {
    lm::core::Template* tmpl = selected_template();
    if (tmpl == nullptr) {
        return;
    }
    const int row = rule_table_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= tmpl->rules.size()) {
        return;
    }

    lm::core::Rule& existing = tmpl->rules[static_cast<std::size_t>(row)];
    AddRuleDialog dialog(this);
    dialog.set_rule(existing);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    lm::core::Rule edited = dialog.rule();
    // The id is kept, never regenerated. It is the join key between this rule
    // and the CheckResults already reported for it, so regenerating would strip
    // the labels off every cached result until each client re-evaluated -- and
    // the id is not a row's identity on screen anyway, the description is.
    edited.id = existing.id;

    spdlog::info("rule edited in template '{}': {} -> {} [{}]", tmpl->name,
                 lm::ui::describe(existing).label.toStdString(),
                 lm::ui::describe(edited).label.toStdString(), edited.id);

    existing = std::move(edited);
    controller_->mark_draft_dirty();
    rebuild_rule_table();
    rule_table_->selectRow(row);
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
    spdlog::info("rule removed from template '{}': {} [{}]", tmpl->name,
                 lm::ui::describe(tmpl->rules[static_cast<std::size_t>(row)]).label.toStdString(),
                 tmpl->rules[static_cast<std::size_t>(row)].id);
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

void FleetWindow::on_assignment_tokens_changed(const QString& host_id, lm::ui::TokenEdit* editor,
                                                const QStringList& templates) {
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(templates.size()));
    QStringList accepted;
    bool created = false;
    bool refused = false;

    for (const QString& name : templates) {
        std::string standard = name.toStdString();

        // The baseline already applies to every host, and it is a field of the
        // bundle rather than one of its templates -- so assigning it is both
        // meaningless and, worse, used to create a *second* template of that
        // name that shadowed it in the list. Dropped, not created.
        if (lm::core::is_baseline_name(standard)) {
            refused = true;
            continue;
        }
        // Naming a template that does not exist creates it. The operator has
        // just said this machine should have it; sending them to the other
        // pane to add it first, then back here, is busywork. An empty template
        // is harmless -- it contributes no rules until one is added, and the
        // yellow text while typing already said it was new.
        const bool exists = std::ranges::any_of(
            controller_->draft().templates,
            [&](const lm::core::Template& tmpl) { return tmpl.name == standard; });
        if (!exists) {
            lm::core::Template tmpl;
            tmpl.name = standard;
            // Worth its own line: a template appearing without anyone visiting
            // the Add Template button is the least obvious edit in this tab.
            spdlog::info("template '{}' created by naming it in an assignment for {}",
                         standard, host_id.toStdString());
            controller_->draft().templates.push_back(std::move(tmpl));
            created = true;
        }
        accepted << QString::fromStdString(standard);
        names.push_back(std::move(standard));
    }

    spdlog::info("assignments for {} set to [{}]", host_id.toStdString(),
                 accepted.join(QStringLiteral(", ")).toStdString());
    controller_->draft().assignments[host_id.toStdString()] = std::move(names);
    controller_->mark_draft_dirty();

    if (refused) {
        // set_tokens() does not emit, so this corrects the chips without
        // re-entering this slot. The chip visibly fails to stick, which with
        // the baseline row saying "always applied" is the whole explanation.
        editor->set_tokens(accepted);
    }
    if (created) {
        rebuild_template_list();
        // Deliberately not rebuild_assignment_table(): this runs from inside
        // the TokenEdit that emitted the signal, and rebuilding would delete
        // it while its own handler is still on the stack.
        refresh_assignment_completions();
    }
}

void FleetWindow::on_assignment_cell_changed(int row, int column) {
    // Column 1 holds a TokenEdit now, not an item, so this only ever concerns
    // the host id -- and it means a rename, which has to move the map entry
    // rather than write a second one under the new key.
    if (updating_assignment_table_ || column != 0) {
        return;
    }
    QTableWidgetItem* host_item = assignment_table_->item(row, 0);
    if (host_item == nullptr) {
        return;
    }

    const QString before = host_item->data(Qt::UserRole).toString();
    const QString after = host_item->text().trimmed();
    if (after == before) {
        return;
    }

    auto& assignments = controller_->draft().assignments;
    if (after.isEmpty() || assignments.contains(after.toStdString())) {
        // A blank id, or one that already has its own row: put the cell back
        // rather than dropping an assignment or silently merging two hosts.
        updating_assignment_table_ = true;
        host_item->setText(before);
        updating_assignment_table_ = false;
        return;
    }

    auto entry = assignments.extract(before.toStdString());
    if (entry.empty()) {
        return;
    }
    entry.key() = after.toStdString();
    assignments.insert(std::move(entry));
    controller_->mark_draft_dirty();

    // Deferred: rebuilding deletes the very item whose setData() is still on
    // the stack above this slot. The row order can change too, since the
    // assignments are a std::map keyed by host id.
    QTimer::singleShot(0, this, &FleetWindow::rebuild_assignment_table);
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
