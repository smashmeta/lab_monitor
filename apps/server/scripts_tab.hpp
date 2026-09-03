#pragma once

#include <QString>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "script_library.hpp"

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTreeWidget;
class QVBoxLayout;

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

    /// Index into the tab's flat list of scripts. Absent on folder rows, which
    /// is how "a script is selected" is decided -- a folder is a category and
    /// has nothing to run.
    static constexpr int kScriptIndexRole = Qt::UserRole + 3;

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

    /// Repaints the preview from whatever the tree now has selected, or clears
    /// it -- a folder row and no row both mean "nothing to show". Also
    /// re-evaluates Run's enabled state, since library mode requires a script.
    void on_script_selection_changed();

private:
    /// Re-reads the share root from the controller and repaints the tree.
    /// Called on construction, on Refresh, and whenever the controller's root
    /// changes -- including the load at startup, which is what actually
    /// delivers a persisted root: at construction time the controller has not
    /// loaded its config yet.
    void reload_library();

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

    // Every widget pointer below is null-initialised rather than left to the
    // constructor to fill in. reload_library() runs from inside the
    // constructor and can, through script_tree_'s currentItemChanged signal,
    // reach update_target_count() -- which touches run_button_ and
    // target_count_label_ -- so a future reordering that calls it before
    // those two exist becomes a loud null dereference here instead of a wild
    // write into whatever garbage the pointer held.
    QStackedWidget* mode_stack_ = nullptr;
    /// The library page's layout. Task 5 adds the preview to it.
    QVBoxLayout* library_page_layout_ = nullptr;

    QLineEdit* share_root_edit_ = nullptr;
    QTreeWidget* script_tree_ = nullptr;
    QLabel* share_message_ = nullptr;
    /// The flat list a tree row's kScriptIndexRole indexes into. Rebuilt from
    /// scratch by every reload_library(), in the same depth-first order the
    /// tree is built in, which is what makes the index and the row agree.
    std::vector<LibraryScript> scripts_;
    /// Read-only preview of whatever script is selected in script_tree_.
    QPlainTextEdit* preview_ = nullptr;
    /// The script backing the preview, or nullopt when nothing selected (or
    /// unreadable) leaves nothing to run. Task 6 re-reads the file at Run and
    /// compares it against previewed_body_ below.
    std::optional<LibraryScript> selected_script_;
    /// The body currently shown in preview_. Empty whenever selected_script_
    /// is empty.
    QString previewed_body_;

    QPlainTextEdit* editor_ = nullptr;
    QListWidget* host_list_ = nullptr;
    QLabel* target_count_label_ = nullptr;
    QPushButton* run_button_ = nullptr;

    QLabel* run_summary_ = nullptr;
    QTableWidget* run_targets_ = nullptr;
    QPlainTextEdit* run_output_ = nullptr;

    /// The run on screen. An id rather than a pointer or an index, because
    /// ServerController's vector of runs reallocates as runs are added and
    /// displayed_run() simply finds nothing if the run is gone.
    std::string displayed_run_id_;

    /// Guards rebuild_host_list() against its own setCheckState() calls
    /// arriving back through QListWidget::itemChanged.
    bool rebuilding_ = false;
};
