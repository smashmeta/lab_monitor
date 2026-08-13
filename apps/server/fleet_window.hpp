#pragma once

#include <QMainWindow>
#include <QMap>
#include <QPoint>
#include <QString>

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
class Sparkline;
class MeterBar;
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

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_selection_changed();
    void on_resource_sample(QString host_id, lm::core::ResourceSample sample);
    void on_compliance_report(QString host_id, lm::core::ComplianceReport report);
    void on_filter_requested(std::optional<lm::core::HostState> state);
    void on_stale_filter_requested(bool active);
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

private:
    void build_fleet_tab();
    void build_templates_tab();
    void refresh_detail_pane(const QString& host_id);
    void populate_compliance_tree(const lm::core::ComplianceReport& report);
    void sync_disk_bars(const std::vector<lm::core::DiskUsage>& disks);
    /// Repopulates the whole Templates tab from the controller's draft. Used
    /// both after an edit and when start() loads persisted config, which
    /// happens after this window has already been constructed.
    void rebuild_templates_view();
    void rebuild_template_list();
    void rebuild_rule_table();
    void rebuild_assignment_table();
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
    QTreeWidget* detail_compliance_tree_;

    QTabWidget* tabs_;

    QListWidget* template_list_;
    QTableWidget* rule_table_;
    QTableWidget* assignment_table_;
    QPushButton* publish_button_;
    QLabel* publish_status_label_;

    /// Guards against QTableWidget::itemChanged firing while
    /// rebuild_assignment_table() is repopulating the table programmatically.
    bool updating_assignment_table_ = false;
};
