#pragma once

#include <QString>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "script_library.hpp"

class QDateEdit;
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

    /// The run id a RunHistoryList row stands for. Public for the same reason
    /// kHostIdRole is: the row's text also carries the script name, a
    /// timestamp and the tally, and anything reading the list back must take
    /// the id from here rather than parse it back out of that text.
    static constexpr int kRunIdRole = Qt::UserRole + 4;

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
    ///
    /// Also updates that run's RunHistoryList row in place -- via
    /// update_run_history_row(), never rebuild_run_history() -- so a tally
    /// stays current on a run nobody has (re)selected without tearing the
    /// whole list down on every one of up to ninety results a run produces.
    /// A run this signal names that the list does not yet have a row for is
    /// left alone rather than rebuilt around. start_script_run() emits
    /// script_run_changed() first and script_runs_changed() immediately
    /// after -- both ordinary direct, same-thread connections -- so on a
    /// brand new run this slot runs while the list is still one statement
    /// short of having the row. The rebuild that follows builds it from the
    /// same text, so there is nothing here to make up for.
    void on_script_run_changed(QString run_id);

    /// Rebuilds RunHistoryList, newest run first, from
    /// controller_->script_runs(). Connected only to
    /// ServerController::script_runs_changed() -- the set of runs changed
    /// shape: one was created, one was loaded at startup, or one (or more)
    /// was deleted -- and called once more at the end of the constructor,
    /// because construction happens before ServerController::start() loads
    /// persisted runs -- see main.cpp.
    ///
    /// Deliberately *not* connected to script_run_changed(), which fires
    /// again for every later result of a run already in the list: rebuilding
    /// on that too would clear() and reconstruct the whole panel once per
    /// result -- about ninety times over a ninety-host run, with the panel
    /// visibly reflowing throughout the one workload this tab exists to
    /// watch. update_run_history_row() carries that case instead.
    ///
    /// Preserves the selection by displayed_run_id_ across a rebuild: a
    /// rebuild driven by an unrelated run must not knock an operator off the
    /// run they are looking at.
    ///
    /// Must never be invoked from a slot connected to RunHistoryList's own
    /// signals (itemChanged, currentRowChanged, ...) -- rebuilding it out from
    /// under itself while it is still on the call stack emitting is the
    /// documented incident the Templates tab's assignment column also guards
    /// against. Deleting through DeleteRunButton is safe because the button,
    /// not a row, is the sender.
    void rebuild_run_history();

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

    /// Drops a pending changed-script block: hides
    /// accept_changed_button_, clears changed_script_pending_ and clears the
    /// banner. Does *not* call update_target_count() -- every caller either
    /// does so itself or is on its way to. Called when the block is accepted
    /// and whenever the comparison behind it stops meaning anything (a new
    /// selection, a reload of the share).
    void clear_changed_script_block();

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
    /// Re-texts the one RunHistoryList row for `run`, found by kRunIdRole,
    /// using the same formatting rebuild_run_history() builds a row with --
    /// so the two cannot drift into disagreeing about how a row reads. A
    /// no-op if the row is not there yet; see on_script_run_changed().
    void update_run_history_row(const ScriptRun& run);
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
    /// Explains a Run press that dispatched nothing -- a share-side change
    /// caught at re-read, or a script that has vanished. Cleared at the start
    /// of every on_run_clicked(), so it never shows a stale reason for a run
    /// that just went out.
    QLabel* run_blocked_message_ = nullptr;
    /// Accepts a body the re-read at Run found changed since the preview.
    /// Hidden until that happens, and hidden again on any new selection, on a
    /// reload of the share, and once pressed.
    ///
    /// A *different* control, deliberately, and not in Run's position. The
    /// block exists because somebody else's edit landed between the preview
    /// and the click; leaving Run enabled and in place makes the reflex
    /// response to "nothing happened" -- press it again -- the whole of the
    /// attack. Continuing has to be a separate, deliberate act on a button
    /// that was not there a moment ago.
    QPushButton* accept_changed_button_ = nullptr;
    /// Set when the re-read at Run found the file changed, cleared only by
    /// accept_changed_button_ or by a selection/reload that throws the
    /// comparison away. Run stays disabled while it holds -- which is why
    /// update_target_count(), the one place Run's enabled state is decided,
    /// has to consult it: ticking a host or switching pages would otherwise
    /// hand the button straight back.
    bool changed_script_pending_ = false;

    QLabel* run_summary_ = nullptr;
    QTableWidget* run_targets_ = nullptr;
    QPlainTextEdit* run_output_ = nullptr;

    /// Past (and in-flight) runs, newest first. Selecting a row sets
    /// displayed_run_id_ and shows it in the run view above -- the same
    /// mechanism a fresh run already used.
    QListWidget* run_history_ = nullptr;
    /// Deletes the selected history row's run. Disabled until a row is
    /// selected, for the same reason RunButton is: a button that does nothing
    /// when pressed reads as broken.
    QPushButton* delete_run_button_ = nullptr;

    /// The cutoff date for bulk cleanup, defaulting to 30 days ago. Read at
    /// DeleteOlderButton's click, never on a timer -- there is deliberately
    /// no automatic pruning of the run history anywhere in this feature.
    QDateEdit* delete_older_date_ = nullptr;
    /// Deletes every run older than delete_older_date_'s chosen day (its
    /// local midnight). No confirmation dialog: this is an explicit,
    /// operator-driven action on a date they typed, and cleanup_message_
    /// reporting the count is the feedback.
    QPushButton* delete_older_button_ = nullptr;
    /// "Deleted N run(s)." after DeleteOlderButton is pressed -- including
    /// N == 0, which is a real answer for a cutoff that matched nothing, not
    /// a silent no-op.
    QLabel* cleanup_message_ = nullptr;

    /// The run on screen. An id rather than a pointer or an index, because
    /// ServerController's vector of runs reallocates as runs are added and
    /// displayed_run() simply finds nothing if the run is gone.
    std::string displayed_run_id_;

    /// Guards rebuild_host_list() against its own setCheckState() calls
    /// arriving back through QListWidget::itemChanged.
    bool rebuilding_ = false;
};
