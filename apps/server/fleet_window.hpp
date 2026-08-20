#pragma once

#include <QMainWindow>
#include <QMap>
#include <QPoint>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include "lm/core/compliance.hpp"
#include "lm/core/fleet.hpp"
#include "lm/core/host_facts.hpp"
#include "lm/core/template_bundle.hpp"

class QCloseEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSplitter;
class QTableView;
class QTableWidget;
class QTabWidget;
class QTreeWidget;
class QVBoxLayout;

class ServerController;
class StatusRibbon;
/// Defined in fleet_window.cpp: a QSortFilterProxyModel over FleetModel that
/// adds a HostState filter and a stale-only filter on top of the base
/// class's own substring filter, so the sidebar's QLineEdit and the
/// StatusRibbon's clicks can both narrow the same view at once.
class FleetProxyModel;

namespace lm::ui {
class AdapterList;
class Sparkline;
class MeterBar;
class TokenEdit;
}  // namespace lm::ui

/// The server's main window, shown at startup (unlike the client's
/// hidden-by-default tray window). Everything here lives on the GUI thread.
/// The host list is a QSortFilterProxyModel wrapping ServerController's
/// FleetModel (severity-sorted, never itself reordered); the centre detail
/// pane and the Templates tab are fed from ServerController's per-sample /
/// per-report signals and its mutable draft() bundle.
class FleetWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit FleetWindow(ServerController* controller, QWidget* parent = nullptr);

    /// The rule kinds Add Rule offers, in the order it offers them.
    ///
    /// Exposed because the dialog itself is a chain of modal prompts no test
    /// can drive, while this list is the part that actually matters: a kind
    /// missing from it is a rule nobody can create through the UI, however
    /// completely the rest of the stack supports it.
    [[nodiscard]] static QStringList rule_kind_choices();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_selection_changed();
    void on_resource_sample(QString host_id, lm::core::ResourceSample sample);
    void on_compliance_report(QString host_id, lm::core::ComplianceReport report);
    void on_filter_requested(std::optional<lm::core::HostState> state);
    void on_stale_filter_requested(bool active);
    /// Drops every filter -- state, stale and the name box -- and clears the
    /// ribbon's active counter.
    void clear_filters();
    /// Refreshes the "Showing N of M - filtered by X" line and the visibility
    /// of the Clear filter button.
    void update_filter_status();
    void on_context_menu_requested(const QPoint& pos);
    void on_publish_clicked();
    void on_draft_publishable_changed(bool can_publish);
    void on_add_expected_host_clicked();

    void on_add_template_clicked();
    void on_remove_template_clicked();
    void on_template_selection_changed();
    void on_add_rule_clicked();
    void on_remove_rule_clicked();
    void on_add_assignment_clicked();
    void on_remove_assignment_clicked();
    void on_assignment_cell_changed(int row, int column);
    /// One host's template chips were edited. Any name that does not match an
    /// existing template creates it — see the definition for why. `editor` is
    /// the widget that emitted, so a refused name can be taken back off it.
    void on_assignment_tokens_changed(const QString& host_id, lm::ui::TokenEdit* editor,
                                       const QStringList& templates);

private:
    void build_fleet_tab();
    /// "12 of 15 rules passed", per host, worst first.
    void build_compliance_tab();
    void rebuild_compliance_table();
    void build_templates_tab();
    void refresh_detail_pane(const QString& host_id);
    void populate_compliance_tree(const lm::core::ComplianceReport& report);
    void sync_disk_bars(const std::vector<lm::core::DiskUsage>& disks);
    /// nullptr when no sample has arrived for the selected host yet. Shows
    /// "not reported" when the host does not advertise Capability::Network,
    /// which is not the same as reporting no adapters.
    void refresh_adapter_list(const lm::core::ResourceSample* sample);
    /// Repopulates the whole Templates tab from the controller's draft. Used
    /// both after an edit and when start() loads persisted config, which
    /// happens after this window has already been constructed.
    void rebuild_templates_view();
    void rebuild_template_list();
    void rebuild_rule_table();
    void rebuild_assignment_table();
    /// Re-offers the current template names to every assignment editor without
    /// recreating them — safe to call from inside one of their signals, which
    /// rebuild_assignment_table() is not.
    void refresh_assignment_completions();
    [[nodiscard]] QStringList template_names() const;
    [[nodiscard]] QString selected_host_id() const;
    /// nullptr only when nothing is selected in template_list_; the
    /// "Baseline" pseudo-entry resolves to &controller_->draft().baseline.
    [[nodiscard]] lm::core::Template* selected_template();
    void save_window_state() const;
    void restore_window_state();

    ServerController* controller_;

    FleetProxyModel* proxy_;
    QTableView* host_view_;
    QLineEdit* filter_edit_;
    QLabel* filter_status_label_ = nullptr;
    QPushButton* clear_filter_button_ = nullptr;
    /// Mirrors what was pushed into the proxy, so the status line can name the
    /// active filters without the proxy having to expose them back.
    std::optional<lm::core::HostState> state_filter_;
    bool stale_filter_ = false;
    /// Spec §11: the only other way to reach ServerController::add_expected_host()
    /// is the Fleet tab's context menu, which only ever offers to add a row
    /// already discovered (i.e. already Online) -- so without this button,
    /// HostState::Missing was unreachable and the ribbon's Missing counter
    /// could never be exercised.
    QPushButton* add_expected_host_button_;
    StatusRibbon* ribbon_;
    QSplitter* main_splitter_;

    QLabel* detail_hostname_label_;
    QLabel* detail_state_label_;
    lm::ui::Sparkline* detail_cpu_sparkline_;
    lm::ui::MeterBar* detail_memory_bar_;
    QVBoxLayout* detail_disk_layout_;
    /// One meter bar per mount point, keyed by DiskUsage::mount, matching
    /// DetailWindow's approach in the client.
    QMap<QString, lm::ui::MeterBar*> detail_disk_bars_;
    lm::ui::AdapterList* detail_adapter_list_;
    QTreeWidget* detail_compliance_tree_;

    QTabWidget* tabs_;

    /// One expanded group per host, listing only what is wrong. Read-only and
    /// oversized on purpose: this tab is written for a wall display.
    QTreeWidget* compliance_tree_;
    QLabel* compliance_summary_label_;

    QListWidget* template_list_;
    QTableWidget* rule_table_;
    QTableWidget* assignment_table_;
    /// Held because it is disabled while the baseline row is selected: the
    /// baseline is part of the bundle and cannot be removed.
    QPushButton* remove_template_button_;
    QPushButton* publish_button_;
    QLabel* publish_status_label_;

    /// Guards against QTableWidget::itemChanged firing while
    /// rebuild_assignment_table() is repopulating the table programmatically.
    bool updating_assignment_table_ = false;
};
