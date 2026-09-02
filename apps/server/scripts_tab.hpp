#pragma once

#include <QWidget>

#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

class ServerController;

/// The Scripts tab: what an operator writes, who it goes to, and Run.
///
/// Phase 1 has no share-backed script picker yet, so the custom-script editor
/// *is* the tab. It opens on a working template rather than an empty box --
/// see kStarterTemplate in the .cpp for why that template is the whole of the
/// LM-RESULT documentation.
///
/// Lives on the GUI thread and reads the fleet straight off ServerController,
/// rebuilding the host list on fleet_changed(). It owns no run state: Run
/// hands everything to ServerController::start_script_run() and the live view
/// (next task) reads it back through script_runs().
class ScriptsTab : public QWidget {
    Q_OBJECT

public:
    explicit ScriptsTab(ServerController* controller, QWidget* parent = nullptr);

    /// The starter template, so a test can assert what the editor opens with
    /// without duplicating it.
    [[nodiscard]] static QString starter_template();

private slots:
    void on_run_clicked();
    /// Repopulates the host list from the controller's fleet, keeping whatever
    /// the operator had already ticked. Cheap enough to run on every
    /// fleet_changed(), which only fires when a host appears, departs or
    /// changes state.
    void rebuild_host_list();

private:
    /// Refreshes "Run on N hosts" and Run's enabled state from the checkboxes.
    void update_target_count();
    /// The ids of every ticked row, in list order.
    [[nodiscard]] std::vector<std::string> checked_hosts() const;

    ServerController* controller_;

    QPlainTextEdit* editor_;
    QListWidget* host_list_;
    QLabel* target_count_label_;
    QPushButton* run_button_;

    /// Guards rebuild_host_list() against its own setCheckState() calls
    /// arriving back through QListWidget::itemChanged.
    bool rebuilding_ = false;
};
