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
#include "lm/ui/keep_foreground_delegate.hpp"
#include "lm/ui/rule_detail.hpp"
#include "lm/ui/meter_bar.hpp"
#include "lm/ui/sparkline.hpp"
#include "lm/ui/theme.hpp"
#include "lm/ui/token_edit.hpp"
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

/// The link states a rule can require. Unknown is deliberately absent: "this
/// adapter must be in a state the client could not determine" is not a check
/// anyone means to write.
const QStringList& link_state_choices() {
    static const QStringList choices{
        QStringLiteral("Up"),          QStringLiteral("No link"),   QStringLiteral("Disconnected"),
        QStringLiteral("Connecting"),  QStringLiteral("Disabled"),  QStringLiteral("Faulted")};
    return choices;
}

lm::core::LinkState link_state_from_choice(const QString& text) {
    if (text == QStringLiteral("Up"))           return lm::core::LinkState::Connected;
    if (text == QStringLiteral("No link"))      return lm::core::LinkState::NoMedia;
    if (text == QStringLiteral("Connecting"))   return lm::core::LinkState::Connecting;
    if (text == QStringLiteral("Disabled"))     return lm::core::LinkState::Disabled;
    if (text == QStringLiteral("Faulted"))      return lm::core::LinkState::Faulted;
    return lm::core::LinkState::Disconnected;
}

/// The DDS matches, in the words to_string(DdsMatch) already uses, so the Add
/// Rule dialog and the rule table read the same. Deliberately not the wire
/// names ("Equals", "AtLeast"): those have to keep parsing out of a saved
/// bundle, while these are free to be reworded.
const QStringList& dds_match_choices() {
    static const QStringList choices{QStringLiteral("equal to"), QStringLiteral("containing"),
                                      QStringLiteral("at least"), QStringLiteral("at most")};
    return choices;
}

lm::core::DdsMatch dds_match_from_choice(const QString& text) {
    if (text == QStringLiteral("containing")) return lm::core::DdsMatch::Contains;
    if (text == QStringLiteral("at least"))   return lm::core::DdsMatch::AtLeast;
    if (text == QStringLiteral("at most"))    return lm::core::DdsMatch::AtMost;
    return lm::core::DdsMatch::Equals;
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
    build_compliance_tab();
    build_templates_tab();

    // The proxy sorts most-urgent-first without the source FleetModel ever
    // reordering its own rows (which stay stable, by host id, for apply()'s
    // insert/remove diffing to work).
    proxy_->sort(0);

    connect(controller_, &ServerController::counts_changed, ribbon_, &StatusRibbon::set_counts);
    // A host going Missing changes the Compliance tab even though no report
    // arrived — that is the whole point of listing silent machines there.
    connect(controller_, &ServerController::fleet_changed, this,
            &FleetWindow::rebuild_compliance_table);
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
    remove_template_button_ = new QPushButton(QStringLiteral("Remove Template"), left);
    template_buttons->addWidget(add_template_button);
    template_buttons->addWidget(remove_template_button_);
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

void FleetWindow::build_compliance_tab() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    compliance_summary_label_ = new QLabel(page);
    QFont summary_font = compliance_summary_label_->font();
    summary_font.setPointSize(summary_font.pointSize() + 3);
    summary_font.setBold(true);
    compliance_summary_label_->setFont(summary_font);
    layout->addWidget(compliance_summary_label_);

    // A tree rather than a table, and read-only: this tab is written for a
    // wall display nobody walks over to. Nothing may hide behind a tooltip, a
    // click or a scroll -- what is wrong has to be on the glass already.
    compliance_tree_ = new QTreeWidget(page);
    compliance_tree_->setColumnCount(2);
    compliance_tree_->setHeaderLabels({QStringLiteral("Host / rule"), QStringLiteral("Observed")});
    compliance_tree_->setRootIsDecorated(false);
    compliance_tree_->setUniformRowHeights(true);
    compliance_tree_->setSelectionMode(QAbstractItemView::NoSelection);
    compliance_tree_->setFocusPolicy(Qt::NoFocus);
    compliance_tree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    compliance_tree_->header()->setStretchLastSection(true);

    // Bigger than the rest of the window: the fleet table is sized to fit ~35
    // rows for someone at the keyboard, this is meant to be read across a room.
    QFont tree_font = compliance_tree_->font();
    tree_font.setPointSize(tree_font.pointSize() + 2);
    compliance_tree_->setFont(tree_font);

    layout->addWidget(compliance_tree_, 1);

    tabs_->addTab(page, QStringLiteral("Compliance"));
    rebuild_compliance_table();
}

namespace {

/// Failing first, then errors, then the ones that could not be checked. Passes
/// never appear individually — see rebuild_compliance_table().
int severity_rank(lm::core::CheckStatus status) {
    switch (status) {
        case lm::core::CheckStatus::Fail:          return 0;
        case lm::core::CheckStatus::Error:         return 1;
        case lm::core::CheckStatus::NotApplicable: return 2;
        case lm::core::CheckStatus::Pass:          return 3;
    }
    return 4;
}

QColor colour_for_status(lm::core::CheckStatus status) {
    switch (status) {
        case lm::core::CheckStatus::Fail:  return QColor(lm::ui::Theme::kMissing);
        case lm::core::CheckStatus::Error: return QColor(lm::ui::Theme::kUnexpected);
        case lm::core::CheckStatus::Pass:  return QColor(lm::ui::Theme::kOnline);
        default:                           return QColor(lm::ui::Theme::kTextMuted);
    }
}

QString glyph_for_status(lm::core::CheckStatus status) {
    switch (status) {
        case lm::core::CheckStatus::Fail:          return QStringLiteral("✕");
        case lm::core::CheckStatus::Error:         return QStringLiteral("!");
        case lm::core::CheckStatus::Pass:          return QStringLiteral("✓");
        case lm::core::CheckStatus::NotApplicable: return QStringLiteral("–");
    }
    return QStringLiteral("?");
}

/// Missing (never seen) and Offline (seen, now quiet) are two ways of saying
/// the same thing to this tab: nothing on screen for that machine is being
/// checked right now.
bool is_silent(lm::core::HostState state) {
    return state == lm::core::HostState::Missing || state == lm::core::HostState::Offline;
}

/// One host as this tab sees it: liveness first, its report second — a silent
/// machine has the first and not the second, which is exactly the case the tab
/// used to drop on the floor.
struct ComplianceGroup {
    QString host;
    lm::core::HostState state = lm::core::HostState::Missing;
    std::optional<lm::core::TimePoint> last_seen;
    const lm::core::ComplianceReport* report = nullptr;
    lm::core::ComplianceSummary summary;
};

/// Silent, then failing, then not-yet-reported, then clean.
///
/// A machine nobody can hear from outranks a broken rule: the rule is a known
/// problem with a known fix, the silence is an unknown. This is also the order
/// the Fleet tab already puts them in, and two views disagreeing about which
/// host is the most urgent is worse than either order.
int group_rank(const ComplianceGroup& group) {
    if (is_silent(group.state)) {
        return 0;
    }
    if (group.report == nullptr) {
        return 2;
    }
    return (group.summary.failing > 0 || group.summary.errors > 0) ? 1 : 3;
}

QString format_last_seen(const std::optional<lm::core::TimePoint>& last_seen) {
    if (!last_seen.has_value()) {
        return QStringLiteral("never");
    }
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(last_seen->time_since_epoch()).count();
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(seconds), Qt::UTC)
        .toString(QStringLiteral("HH:mm:ss"));
}

}  // namespace

void FleetWindow::rebuild_compliance_table() {
    const QMap<QString, lm::core::ComplianceReport>& reports = controller_->report_cache();
    compliance_tree_->clear();

    // Driven by the fleet, not by the report cache. The cache only ever grows,
    // so a host that has gone would keep its last score on the wall forever,
    // and a host that has never reported would never appear at all.
    std::vector<ComplianceGroup> groups;
    groups.reserve(controller_->fleet().entries.size());
    for (const lm::core::FleetEntry& entry : controller_->fleet().entries) {
        ComplianceGroup group;
        group.host = QString::fromStdString(entry.host_id);
        group.state = entry.state;
        group.last_seen = entry.last_seen;
        // A silent host's last report describes a machine that is no longer
        // answering; it is reported as history below, never as a live score.
        const auto cached = reports.constFind(group.host);
        if (cached != reports.constEnd()) {
            group.report = &cached.value();
            group.summary = lm::core::summarise(*group.report);
        }
        groups.push_back(std::move(group));
    }

    // Worst first: the point of this tab is finding what needs attention, and a
    // host with three failures should not be below one with none because its
    // name sorts later.
    std::ranges::sort(groups, [](const ComplianceGroup& lhs, const ComplianceGroup& rhs) {
        if (group_rank(lhs) != group_rank(rhs)) {
            return group_rank(lhs) < group_rank(rhs);
        }
        if (lhs.summary.failing != rhs.summary.failing) {
            return lhs.summary.failing > rhs.summary.failing;
        }
        if (lhs.summary.errors != rhs.summary.errors) {
            return lhs.summary.errors > rhs.summary.errors;
        }
        return lhs.host < rhs.host;
    });

    std::size_t total_failing = 0;
    std::size_t fully_compliant = 0;
    std::size_t silent = 0;

    for (const ComplianceGroup& group : groups) {
        const QString& host = group.host;
        const lm::core::ComplianceSummary summary = group.summary;

        auto* host_item = new QTreeWidgetItem(compliance_tree_);
        host_item->setText(0, host);
        QFont host_font = compliance_tree_->font();
        host_font.setBold(true);
        host_item->setFont(0, host_font);
        host_item->setFont(1, host_font);
        host_item->setExpanded(true);

        if (is_silent(group.state)) {
            ++silent;
            const bool never = group.state == lm::core::HostState::Missing;
            host_item->setText(1, never ? QStringLiteral("Missing — never reported")
                                        : QStringLiteral("Offline — last seen %1")
                                              .arg(format_last_seen(group.last_seen)));
            // Same hue the Fleet tab paints it, so the two cannot describe the
            // same machine differently.
            const QColor colour = lm::ui::FleetModel::colour_for(
                never ? lm::ui::FleetModel::RowHealth::Missing
                      : lm::ui::FleetModel::RowHealth::Offline);
            host_item->setForeground(0, colour);
            host_item->setForeground(1, colour);

            auto* row = new QTreeWidgetItem(host_item);
            // Never "0 / 0 rules passed", which reads as a clean bill of health
            // from across a room. Its last score is history, and labelled so.
            row->setText(0, QStringLiteral("✕  Not reporting — no rules are being checked"));
            if (group.report != nullptr) {
                row->setText(1, QStringLiteral("last known %1 / %2 rules passed")
                                    .arg(summary.passed)
                                    .arg(summary.checked()));
            }
            row->setForeground(0, colour);
            row->setForeground(1, colour);
            continue;
        }

        if (group.report == nullptr) {
            host_item->setText(1, QStringLiteral("No compliance report yet"));
            const QColor colour = lm::ui::FleetModel::colour_for(
                lm::ui::FleetModel::RowHealth::Unknown);
            host_item->setForeground(0, colour);
            host_item->setForeground(1, colour);

            auto* row = new QTreeWidgetItem(host_item);
            row->setText(0, QStringLiteral("–  Reporting, but has not evaluated its rules yet"));
            row->setForeground(0, colour);
            row->setForeground(1, colour);
            continue;
        }

        const lm::core::ComplianceReport& report = *group.report;
        total_failing += summary.failing;
        if (summary.failing == 0 && summary.errors == 0) {
            ++fully_compliant;
        }

        host_item->setText(1, QStringLiteral("%1 / %2 rules passed")
                                   .arg(summary.passed)
                                   .arg(summary.checked()));

        const QColor host_colour =
            lm::ui::Theme::color_for_load(100.0 * (1.0 - summary.passed_ratio()));
        host_item->setForeground(0, host_colour);
        host_item->setForeground(1, host_colour);

        // Rule descriptions live in the bundle this server published, recovered
        // the same way the detail pane does -- and per host, since rules_for()
        // is what decides which rules this machine was even given.
        QHash<QString, lm::ui::RuleDetail> by_id;
        for (const lm::core::Rule* rule :
             lm::core::rules_for(controller_->published(), host.toStdString())) {
            by_id.insert(QString::fromStdString(rule->id), lm::ui::describe(*rule));
        }

        std::vector<const lm::core::CheckResult*> ordered;
        ordered.reserve(report.results.size());
        for (const lm::core::CheckResult& result : report.results) {
            ordered.push_back(&result);
        }
        std::ranges::sort(ordered, [&](const lm::core::CheckResult* lhs,
                                        const lm::core::CheckResult* rhs) {
            if (severity_rank(lhs->status) != severity_rank(rhs->status)) {
                return severity_rank(lhs->status) < severity_rank(rhs->status);
            }
            return lhs->rule_id < rhs->rule_id;
        });

        for (const lm::core::CheckResult* result : ordered) {
            // Passing rules are counted, never listed. On a shared display a
            // wall of green pushes the two red lines that matter off the
            // screen; the host row already says how many passed.
            if (result->status == lm::core::CheckStatus::Pass) {
                continue;
            }

            const QString id = QString::fromStdString(result->rule_id);
            const auto detail = by_id.constFind(id);
            const QString label = detail != by_id.constEnd() ? detail->label : id;

            auto* row = new QTreeWidgetItem(host_item);
            row->setText(0, QStringLiteral("%1  %2").arg(glyph_for_status(result->status), label));

            QString observed = QString::fromStdString(result->observed);
            const QString message = QString::fromStdString(result->message);
            if (!message.isEmpty() && message != observed) {
                observed = observed.isEmpty() ? message
                                              : QStringLiteral("%1 - %2").arg(observed, message);
            }
            row->setText(1, observed);

            const QColor colour = colour_for_status(result->status);
            row->setForeground(0, colour);
            row->setForeground(1, colour);
        }

        // Only when nothing else is listed, which is the entire purpose of this
        // line: an empty group reads as "no data" from across a room. A host
        // whose group already holds its not-applicable rules does not need to
        // be told again that the rest passed -- the host row says so.
        if (host_item->childCount() == 0) {
            auto* row = new QTreeWidgetItem(host_item);
            row->setText(0, QStringLiteral("%1  All %2 checked rules passing")
                                 .arg(glyph_for_status(lm::core::CheckStatus::Pass))
                                 .arg(summary.checked()));
            row->setForeground(0, QColor(lm::ui::Theme::kOnline));
            row->setForeground(1, QColor(lm::ui::Theme::kOnline));
        }
    }

    compliance_tree_->resizeColumnToContents(0);

    QString headline = QStringLiteral("%1 of %2 hosts fully compliant · %3 failing checks")
                           .arg(fully_compliant)
                           .arg(groups.size())
                           .arg(total_failing);
    if (silent > 0) {
        headline += QStringLiteral(" · %1 not reporting").arg(silent);
    }
    compliance_summary_label_->setText(
        groups.empty() ? QStringLiteral("No host has reported compliance yet") : headline);
    // Silence counts as trouble here: an unreachable machine is a red line on
    // the wall, not a quiet one.
    compliance_summary_label_->setStyleSheet(
        QStringLiteral("color: %1;")
            .arg(total_failing == 0 && silent == 0 ? lm::ui::Theme::kOnline
                                                   : lm::ui::Theme::kMissing));
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
    // The summary tab covers every host, so it refreshes whoever reported --
    // unlike the detail pane below, which only concerns the selected one.
    rebuild_compliance_table();

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
        rule_table_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(rule.id)));
        rule_table_->setItem(row, 1, new QTableWidgetItem(kind_label(lm::core::kind_of(rule))));
        // Through describe(), not from rule.expectation directly. Two kinds
        // carry their own direction -- the adapter count in its comparison, the
        // DDS value in its match -- and Add Rule does not ask them for a
        // presence at all. Printing the default "Must be present" beside
        // "at least 2" put a second, contradictable answer in the row for a
        // question already settled; describe() shows the constraint instead.
        const lm::ui::RuleDetail detail = lm::ui::describe(rule);
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
    controller_->mark_draft_dirty();
    rebuild_template_list();
    rebuild_rule_table();
    // Assignments naming it are left alone: rules_for() already ignores a name
    // with no template behind it, and silently editing the operator's
    // assignments because they deleted something is worse than a stale chip.
    refresh_assignment_completions();
}

QStringList FleetWindow::rule_kind_choices() {
    return {QStringLiteral("Process"),
            QStringLiteral("Service"),
            QStringLiteral("Registry"),
            QStringLiteral("Network: adapter count"),
            QStringLiteral("Network: named adapter"),
            QStringLiteral("DDS: topic is published"),
            QStringLiteral("DDS: value on a topic")};
}

void FleetWindow::on_add_rule_clicked() {
    lm::core::Template* tmpl = selected_template();
    if (tmpl == nullptr) {
        QMessageBox::information(this, QStringLiteral("Add Rule"), QStringLiteral("Select a template first."));
        return;
    }

    // No id prompt: ids are generated from the rule once its payload is known
    // (see the end of this function). They are a join key between a rule and
    // the CheckResult reported for it, not something an operator should have to
    // keep a ledger of -- and a reused one silently cost a rule, since
    // rules_for() keeps only the first holder of an id.
    bool ok = false;
    const QStringList kinds = rule_kind_choices();
    const QString kind =
        QInputDialog::getItem(this, QStringLiteral("Rule Kind"), QStringLiteral("Kind:"), kinds, 0, false, &ok);
    if (!ok) {
        return;
    }

    // Two kinds carry their own direction -- the adapter count in its
    // comparison, the DDS value in its match -- so asking for a presence on top
    // would be a second, contradictable way of saying the same thing. Skipped
    // rather than asked and ignored.
    const bool asks_expectation = kind != QStringLiteral("Network: adapter count") &&
                                  kind != QStringLiteral("DDS: value on a topic");
    QString expectation_text = QStringLiteral("Must be present");
    if (asks_expectation) {
        const QStringList expectations{QStringLiteral("Must be present"),
                                        QStringLiteral("Must be absent")};
        expectation_text = QInputDialog::getItem(this, QStringLiteral("Expectation"),
                                                  QStringLiteral("Expectation:"), expectations, 0,
                                                  false, &ok);
        if (!ok) {
            return;
        }
    }

    const QString description = QInputDialog::getText(
        this, QStringLiteral("Description"), QStringLiteral("Description (optional):"), QLineEdit::Normal, {}, &ok);
    if (!ok) {
        return;
    }

    lm::core::Rule rule;
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
    } else if (kind == QStringLiteral("Network: adapter count")) {
        const QStringList comparisons{QStringLiteral("at least"), QStringLiteral("exactly"),
                                       QStringLiteral("at most")};
        const QString comparison_text =
            QInputDialog::getItem(this, QStringLiteral("Connected Adapters"),
                                   QStringLiteral("How many adapters must be connected?"),
                                   comparisons, 0, false, &ok);
        if (!ok) {
            return;
        }
        const int count = QInputDialog::getInt(this, QStringLiteral("Connected Adapters"),
                                                QStringLiteral("Count:"), 1, 0, 64, 1, &ok);
        if (!ok) {
            return;
        }
        lm::core::AdapterCountRule payload;
        payload.comparison = comparison_text == QStringLiteral("exactly") ? lm::core::Comparison::Exactly
                             : comparison_text == QStringLiteral("at most")
                                 ? lm::core::Comparison::AtMost
                                 : lm::core::Comparison::AtLeast;
        payload.count = count;
        rule.payload = payload;
    } else if (kind == QStringLiteral("Network: named adapter")) {
        // Offered as free text rather than a picker: the server does not know
        // which adapters a host has until that host reports, and a rule is
        // routinely written for machines that have not checked in yet.
        const QString name = QInputDialog::getText(
            this, QStringLiteral("Adapter"),
            QStringLiteral("Adapter name, as Network Connections shows it:"), QLineEdit::Normal, {},
            &ok);
        if (!ok || name.trimmed().isEmpty()) {
            return;
        }
        const QString state_text =
            QInputDialog::getItem(this, QStringLiteral("Adapter Link"),
                                   QStringLiteral("Required link state:"), link_state_choices(), 0,
                                   false, &ok);
        if (!ok) {
            return;
        }
        rule.payload = lm::core::AdapterStateRule{name.trimmed().toStdString(),
                                                   link_state_from_choice(state_text)};
    } else if (kind == QStringLiteral("DDS: topic is published") ||
               kind == QStringLiteral("DDS: value on a topic")) {
        // Both DDS kinds start the same way, so the domain and topic are asked
        // once here rather than duplicated down two branches.
        const int domain_id = QInputDialog::getInt(this, QStringLiteral("DDS Domain"),
                                                    QStringLiteral("Domain id:"), 0, 0, 232, 1, &ok);
        if (!ok) {
            return;
        }
        const QString topic = QInputDialog::getText(this, QStringLiteral("DDS Topic"),
                                                     QStringLiteral("Topic name:"), QLineEdit::Normal,
                                                     {}, &ok);
        if (!ok || topic.trimmed().isEmpty()) {
            return;
        }

        if (kind == QStringLiteral("DDS: topic is published")) {
            rule.payload = lm::core::DdsTopicRule{static_cast<std::uint32_t>(domain_id),
                                                   topic.trimmed().toStdString()};
        } else {
            // Free text, and it has to be: the server has never seen this type
            // and cannot offer its fields. The help line carries the whole
            // grammar, because there is nowhere else an operator would find it.
            const QString path = QInputDialog::getText(
                this, QStringLiteral("Value"),
                QStringLiteral("Path into the sample — e.g. status, items_.length, items_[0].sku:"),
                QLineEdit::Normal, {}, &ok);
            if (!ok || path.trimmed().isEmpty()) {
                return;
            }
            const QString match_text =
                QInputDialog::getItem(this, QStringLiteral("Match"), QStringLiteral("The value must be:"),
                                       dds_match_choices(), 0, false, &ok);
            if (!ok) {
                return;
            }
            const QString expected =
                QInputDialog::getText(this, QStringLiteral("Expected Value"),
                                       QStringLiteral("Expected value:"), QLineEdit::Normal, {}, &ok);
            if (!ok || expected.trimmed().isEmpty()) {
                return;
            }

            lm::core::DdsValueRule payload;
            payload.domain_id = static_cast<std::uint32_t>(domain_id);
            payload.topic_name = topic.trimmed().toStdString();
            payload.path = path.trimmed().toStdString();
            payload.match = dds_match_from_choice(match_text);
            payload.expected_value = expected.trimmed().toStdString();
            rule.payload = payload;
        }
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

    // Last, because the id is derived from the payload -- and against the whole
    // draft, since templates are combined per host and an id taken in any of
    // them is taken here too.
    rule.id = lm::core::make_rule_id(controller_->draft(), rule);

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
            controller_->draft().templates.push_back(std::move(tmpl));
            created = true;
        }
        accepted << QString::fromStdString(standard);
        names.push_back(std::move(standard));
    }

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
