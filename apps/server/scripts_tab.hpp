#pragma once

#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

class ServerController;
struct ScriptRun;
struct RunTarget;

/// The Scripts tab: what an operator writes, who it goes to, and Run.
///
/// Phase 1 has no share-backed script picker yet, so the custom-script editor
/// *is* the tab. It opens on a working template rather than an empty box --
/// see kStarterTemplate in the .cpp for why that template is the whole of the
/// LM-RESULT documentation.
///
/// Lives on the GUI thread and reads the fleet straight off ServerController,
/// rebuilding the host list on fleet_changed(). It owns no run state: Run
/// hands everything to ServerController::start_script_run(), and the live run
/// view below reads it back through script_runs(), holding nothing but the id
/// of the run it is displaying.
class ScriptsTab : public QWidget {
    Q_OBJECT

public:
    explicit ScriptsTab(ServerController* controller, QWidget* parent = nullptr);

    /// The host id a row stands for. Public because the row's *text* also
    /// carries a suffix saying what to know about that machine, and anything
    /// reading the list back -- the tests included -- must take the id from
    /// here rather than parse the suffix off again.
    static constexpr int kHostIdRole = Qt::UserRole + 1;

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

    /// Refreshes the run view, but only when the run that moved is the one on
    /// screen. script_run_changed fires for every result of every run this
    /// server has issued, and a run nobody is looking at must not repaint.
    void on_script_run_changed(QString run_id);

private:
    /// Refreshes "Run on N hosts" and Run's enabled state from the checkboxes.
    void update_target_count();
    /// The ids of every ticked row, in list order.
    [[nodiscard]] std::vector<std::string> checked_hosts() const;

    /// The run named by displayed_run_id_, or null when there is none yet.
    [[nodiscard]] const ScriptRun* displayed_run() const;
    /// Repaints the tally, the target rows and the output pane from the
    /// displayed run. Cells are updated in place rather than replaced, so a
    /// result arriving for one host leaves the selection -- and therefore the
    /// output pane -- where the operator put it.
    void refresh_run_view();
    /// Fills the output pane from whichever row is selected, or explains why
    /// there is nothing to show.
    void update_run_output();
    /// The target the selected row stands for, or null when nothing is
    /// selected.
    [[nodiscard]] const RunTarget* selected_target() const;

    ServerController* controller_;

    QPlainTextEdit* editor_;
    QListWidget* host_list_;
    QLabel* target_count_label_;
    QPushButton* run_button_;

    QLabel* run_summary_;
    QTableWidget* run_targets_;
    QPlainTextEdit* run_output_;

    /// The run on screen. An id rather than a pointer or an index, because
    /// ServerController's vector of runs reallocates as runs are added and
    /// displayed_run() simply finds nothing if the run is gone.
    std::string displayed_run_id_;

    /// Guards rebuild_host_list() against its own setCheckState() calls
    /// arriving back through QListWidget::itemChanged.
    bool rebuilding_ = false;
};
