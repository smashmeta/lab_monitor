#include "scripts_tab.hpp"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "lm/core/fleet.hpp"
#include "lm/core/types.hpp"
#include "lm/ui/keep_foreground_delegate.hpp"
#include "lm/ui/theme.hpp"
#include "script_run.hpp"
#include "server_controller.hpp"

namespace {

/// The editor's starting content, verbatim from the design spec.
///
/// Three things about it are deliberate and must survive an edit here.
///
/// It **runs as-is and does nothing**, so pressing Run on an untouched
/// template is safe and demonstrates the whole path -- dispatch, capture,
/// verdict -- before anybody has written a line of their own.
///
/// It shows **both** branches, because the failure one is what people get
/// wrong: a script that catches its own error and forgets to exit non-zero
/// reports success, and PowerShell will happily let it.
///
/// It builds its JSON with ConvertTo-Json rather than a hand-built string,
/// because hand-quoting JSON inside PowerShell is a reliable source of
/// malformed markers -- and a malformed marker is silently ignored, so the
/// mistake costs an operator the one line they wrote the script to see.
///
/// This is also the only place the LM-RESULT convention is taught. A page of
/// documentation describing it would be read by nobody; boilerplate that
/// already does it correctly gets edited rather than replaced.
constexpr const char* kStarterTemplate = R"PS(# Runs on each selected host as the lab_monitor agent.
# The exit code decides the outcome: 0 = Completed, anything else = Failed.
# The LM-RESULT line is optional — it lets you say *why*, in the run view.

function Report($ok, $message) {
    $payload = @{ ok = $ok; message = $message } | ConvertTo-Json -Compress
    Write-Output "LM-RESULT: $payload"
}

try {
    # --- your work here ---
    Write-Output "Nothing to do yet."

    # Audible proof this ran on the machine you targeted: one high note for
    # success, one low one for failure. Two sounds that cannot be mistaken
    # for each other, so a row of PCs can be checked by ear. Delete both
    # when you put real work here.
    [console]::Beep(1047, 400)
    Report $true "completed"
    exit 0
}
catch {
    [console]::Beep(196, 700)
    Report $false $_.Exception.Message
    exit 1
}
)PS";

/// Phase 1 has no per-run timeout control, and 120 s matches ScriptRun's own
/// default. Named rather than inlined so the run view and this agree when the
/// control arrives.
constexpr std::uint32_t kDefaultTimeoutSeconds = 120;

/// The name a run made from the editor is recorded under. Custom runs are as
/// auditable as named ones -- the body is kept verbatim either way -- so this
/// says where it came from rather than pretending to be a filename.
const QString kCustomScriptName = QStringLiteral("(custom script)");

/// What a row says about a host beyond its name; empty when there is nothing
/// to say.
///
/// Several different things wear the same suffix, deliberately: they all
/// answer "what should I know about this machine before I tick it", and
/// splitting them into a note and a warning would make an operator learn two
/// visual languages for one question. None of them stops the row being
/// selected -- the server decides what it will actually dispatch, and says so
/// per target in the run view.
/// The wording is the tab's own -- short enough to sit at the end of a row --
/// where ServerController::start_script_run() records a fuller sentence on the
/// run itself ("host is Offline, not Online", "not enrolled for script
/// execution"). The reasons correspond; nothing enforces that the phrasing
/// does, so do not read one off the other.
QString note_for(const lm::core::FleetEntry& entry) {
    if (entry.state != lm::core::HostState::Online) {
        return QString::fromStdString(lm::core::to_string(entry.state));
    }
    if (!entry.caps.has(lm::core::Capability::Scripts)) {
        return QStringLiteral("not enrolled");
    }
    if (!entry.caps.has(lm::core::Capability::Elevated)) {
        // Marked, not refused. Phase 1 has no way to ask for elevation and
        // plenty of scripts need no admin, so this host runs them perfectly
        // well -- the flag is here so an access-denied is read *before* the
        // run rather than as a column of failures afterwards.
        return QStringLiteral("not elevated");
    }
    return {};
}

/// The colour an outcome is painted in.
///
/// Every one of these comes from lm::ui::Theme's existing status palette
/// rather than a set invented here, so a red in the run view and a red in the
/// fleet table cannot come to mean different things. The *word* is still the
/// signal -- "Completed" reads as itself in greyscale -- and the colour only
/// makes the one row worth acting on findable at a glance.
///
/// Pending is the muted grey of a row nothing has happened to yet. Dispatched
/// takes kPaused, the blue that already means "somebody chose this, it is
/// neither an alarm nor healthy" in the fleet table -- it cannot share the grey
/// with NoResponse, because kTextMuted and kNotApplicable are the same value
/// and "still waiting" and "gave up waiting" would then be indistinguishable.
/// NoResponse is the row an operator most needs to find on a ninety-row table.
QColor colour_for(TargetState state) {
    switch (state) {
        case TargetState::Pending:
            return QColor(lm::ui::Theme::kTextMuted);
        case TargetState::Dispatched:
            return QColor(lm::ui::Theme::kPaused);
        case TargetState::Completed:
            return QColor(lm::ui::Theme::kOnline);
        case TargetState::Failed:
            return QColor(lm::ui::Theme::kMissing);
        case TargetState::Refused:
            return QColor(lm::ui::Theme::kOffline);
        case TargetState::NoResponse:
            return QColor(lm::ui::Theme::kNotApplicable);
    }
    return QColor(lm::ui::Theme::kText);
}

/// What the Outcome column says.
///
/// to_string(TargetState) is the wire and log spelling and stays exactly as it
/// is -- other tests assert its values verbatim. Only NoResponse reads badly to
/// an operator, so the view spells that one itself rather than reshaping a
/// helper that is not the view's to own.
QString display_name(TargetState state) {
    switch (state) {
        case TargetState::NoResponse:
            return QStringLiteral("No response");
        case TargetState::Pending:
        case TargetState::Dispatched:
        case TargetState::Completed:
        case TargetState::Failed:
        case TargetState::Refused:
            break;
    }
    return QString::fromStdString(to_string(state));
}

/// The one-line "why" beside an outcome: the refusal reason, else whatever the
/// script itself said in its LM-RESULT line, else the exit code when it is not
/// zero. Empty when there is nothing to add -- a successful run with nothing to
/// report should not have to fill a column.
QString detail_for(const RunTarget& target) {
    if (!target.detail.empty()) {
        return QString::fromStdString(target.detail);
    }
    if (!target.result.has_value()) {
        return {};
    }
    const lm::transport::ScriptResultMessage& result = *target.result;
    if (result.has_reported && !result.reported_message.empty()) {
        return QString::fromStdString(result.reported_message);
    }
    if (result.exit_code != 0) {
        return QStringLiteral("exit %1").arg(result.exit_code);
    }
    return {};
}

/// `12 completed · 2 failed · 1 no response`.
///
/// Only the non-zero counts appear: on a run where every host succeeded, four
/// zeroes alongside the one number that matters is noise, and the reader has
/// to find the real figure among them. Outcomes lead, because they are what is
/// being waited for; what is still in flight follows.
QString summarise(const RunTally& tally) {
    QStringList parts;
    const auto add = [&parts](std::size_t count, const char* noun) {
        if (count > 0) {
            parts << QStringLiteral("%1 %2").arg(QString::number(static_cast<qulonglong>(count)),
                                                 QString::fromLatin1(noun));
        }
    };
    add(tally.completed, "completed");
    add(tally.failed, "failed");
    add(tally.refused, "refused");
    add(tally.no_response, "no response");
    add(tally.dispatched, "dispatched");
    add(tally.pending, "pending");
    return parts.join(QStringLiteral(" · "));
}

/// What to say for a target that has no result yet. Its own function so the
/// switch stays exhaustive: the three answered states cannot reach here --
/// ScriptRun::apply_result() always stores the message that moved them -- and
/// saying so once beats a `default:` that would swallow a future state.
QString waiting_text(const RunTarget& target) {
    const QString host = QString::fromStdString(target.host_id);
    switch (target.state) {
        case TargetState::Pending:
            return QStringLiteral("%1 has not been sent the script yet.").arg(host);
        case TargetState::Dispatched:
            return QStringLiteral("Waiting for %1 to report back…").arg(host);
        case TargetState::NoResponse:
            return QStringLiteral("%1 did not report back before the deadline.").arg(host);
        case TargetState::Completed:
        case TargetState::Failed:
        case TargetState::Refused:
            break;
    }
    return QStringLiteral("%1 reported nothing.").arg(host);
}

/// Everything known about one target, for the output pane.
QString output_for(const RunTarget& target) {
    const QString host = QString::fromStdString(target.host_id);

    if (target.state == TargetState::Refused) {
        // A refused host has no output -- nothing ran. An empty pane here
        // would read as "ran and printed nothing", which is a different and
        // far more worrying thing, so the reason takes output's place.
        //
        // detail is the only place to look: refuse_at_dispatch() always passes
        // a reason, and apply_result() copies refusal_reason into detail for
        // every Refused result, so a second read of the message itself could
        // never say anything this does not.
        return QStringLiteral("%1 was not asked to run this script.\n\n%2")
            .arg(host, target.detail.empty() ? QStringLiteral("No reason was given.")
                                             : QString::fromStdString(target.detail));
    }
    if (!target.result.has_value()) {
        return waiting_text(target);
    }

    const lm::transport::ScriptResultMessage& result = *target.result;
    QStringList lines;
    lines << QStringLiteral("%1 — %2, exit code %3, %4 ms")
                 .arg(host, display_name(target.state),
                      QString::number(result.exit_code),
                      QString::number(static_cast<qulonglong>(result.duration_ms)));
    if (result.has_reported) {
        lines << QStringLiteral("LM-RESULT: %1 — %2")
                     .arg(result.reported_ok ? QStringLiteral("ok") : QStringLiteral("not ok"),
                          QString::fromStdString(result.reported_message));
    }
    if (!result.stdout_text.empty()) {
        lines << QString() << QString::fromStdString(result.stdout_text);
    }
    if (!result.stderr_text.empty()) {
        lines << QString() << QStringLiteral("stderr:")
              << QString::fromStdString(result.stderr_text);
    }
    if (result.stdout_text.empty() && result.stderr_text.empty()) {
        // Said explicitly, because a script that printed nothing and one whose
        // output was lost look identical in a blank pane.
        lines << QString() << QStringLiteral("(no output)");
    }
    return lines.join(QLatin1Char('\n'));
}

/// Writes a cell, reusing the item already there.
///
/// Not setItem(): replacing an item drops the row's selection with it, and a
/// result arriving for one host would then throw away whichever row the
/// operator had opened in the output pane.
void set_cell(QTableWidget* table, int row, int column, const QString& text,
              const QColor& colour) {
    QTableWidgetItem* item = table->item(row, column);
    if (item == nullptr) {
        item = new QTableWidgetItem;
        table->setItem(row, column, item);
    }
    item->setText(text);
    item->setForeground(colour);
}

}  // namespace

QString ScriptsTab::starter_template() {
    return QString::fromUtf8(kStarterTemplate);
}

ScriptsTab::ScriptsTab(ServerController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller) {
    auto* layout = new QVBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);

    auto* script_side = new QWidget(splitter);
    auto* script_layout = new QVBoxLayout(script_side);
    script_layout->setContentsMargins(0, 0, 0, 0);
    script_layout->addWidget(new QLabel(QStringLiteral("Script"), script_side));

    mode_stack_ = new QStackedWidget(script_side);
    mode_stack_->setObjectName(QStringLiteral("ScriptModeStack"));

    // Page 0: the library. Tasks 4 and 5 fill library_page_layout_; the switch
    // button lives here from the start so the two pages work immediately.
    auto* library_page = new QWidget(mode_stack_);
    library_page_layout_ = new QVBoxLayout(library_page);
    library_page_layout_->setContentsMargins(0, 0, 0, 0);
    auto* to_custom = new QPushButton(QStringLiteral("Custom script…"), library_page);
    to_custom->setObjectName(QStringLiteral("CustomScriptButton"));
    library_page_layout_->addWidget(to_custom);
    mode_stack_->addWidget(library_page);

    // Page 1: the editor that was the whole tab in phase 1.
    auto* editor_page = new QWidget(mode_stack_);
    auto* editor_layout = new QVBoxLayout(editor_page);
    editor_layout->setContentsMargins(0, 0, 0, 0);
    auto* back = new QPushButton(QStringLiteral("Back to script list"), editor_page);
    back->setObjectName(QStringLiteral("BackToListButton"));
    editor_layout->addWidget(back);
    editor_ = new QPlainTextEdit(editor_page);
    editor_->setObjectName(QStringLiteral("ScriptEditor"));
    // Fixed-width for the same reason the shopping cart's path pane is: in the
    // proportional UI font PowerShell's punctuation runs together, and this is
    // text somebody has to read character by character before running it on a
    // hundred machines.
    editor_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor_->setPlainText(starter_template());
    editor_layout->addWidget(editor_, 1);

    auto* reset_button = new QPushButton(QStringLiteral("Reset to template"), editor_page);
    reset_button->setObjectName(QStringLiteral("ResetTemplateButton"));
    auto* script_buttons = new QHBoxLayout();
    script_buttons->addWidget(reset_button);
    script_buttons->addStretch(1);
    editor_layout->addLayout(script_buttons);
    mode_stack_->addWidget(editor_page);

    mode_stack_->setCurrentIndex(0);  // the list is the default view
    script_layout->addWidget(mode_stack_, 1);

    connect(to_custom, &QPushButton::clicked, this, [this] { mode_stack_->setCurrentIndex(1); });
    connect(back, &QPushButton::clicked, this, [this] { mode_stack_->setCurrentIndex(0); });

    splitter->addWidget(script_side);

    auto* host_side = new QWidget(splitter);
    auto* host_layout = new QVBoxLayout(host_side);
    host_layout->setContentsMargins(0, 0, 0, 0);
    host_layout->addWidget(new QLabel(QStringLiteral("Hosts"), host_side));

    host_list_ = new QListWidget(host_side);
    host_list_->setObjectName(QStringLiteral("HostList"));
    host_layout->addWidget(host_list_, 1);

    // Takes every row, including the ones a note marks as unable to comply.
    // The note is the warning; the server is the gate, and it refuses at
    // dispatch with a reason rather than sending anything to such a host. A
    // button that silently skipped rows would be the surprise, since the one
    // thing "Select all" can be read to mean is all of them.
    auto* select_all_button = new QPushButton(QStringLiteral("Select all"), host_side);
    select_all_button->setObjectName(QStringLiteral("SelectAllButton"));
    auto* clear_button = new QPushButton(QStringLiteral("Clear"), host_side);
    clear_button->setObjectName(QStringLiteral("ClearButton"));
    auto* host_buttons = new QHBoxLayout();
    host_buttons->addWidget(select_all_button);
    host_buttons->addWidget(clear_button);
    host_buttons->addStretch(1);
    host_layout->addLayout(host_buttons);
    splitter->addWidget(host_side);

    // The editor is the working surface; the host list is a column of names.
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);

    // Composing a run and watching one are stacked rather than tabbed: the
    // second follows straight from the first, and an operator who has just
    // pressed Run should not have to go and find the answer.
    auto* vertical = new QSplitter(Qt::Vertical, this);

    auto* compose_side = new QWidget(vertical);
    auto* compose_layout = new QVBoxLayout(compose_side);
    compose_layout->setContentsMargins(0, 0, 0, 0);
    compose_layout->addWidget(splitter, 1);

    // The count sits beside Run because the blast radius must be readable
    // without counting checkboxes -- this is the last thing seen before code
    // goes out to other people's machines.
    target_count_label_ = new QLabel(compose_side);
    target_count_label_->setObjectName(QStringLiteral("TargetCountLabel"));
    run_button_ = new QPushButton(QStringLiteral("Run"), compose_side);
    run_button_->setObjectName(QStringLiteral("RunButton"));
    auto* run_row = new QHBoxLayout();
    run_row->addStretch(1);
    run_row->addWidget(target_count_label_);
    run_row->addWidget(run_button_);
    compose_layout->addLayout(run_row);
    vertical->addWidget(compose_side);

    auto* run_side = new QWidget(vertical);
    auto* run_layout = new QVBoxLayout(run_side);
    run_layout->setContentsMargins(0, 0, 0, 0);

    // Above the rows, not below and not in a status bar: on a run of ninety
    // machines the counts are what is read, and the rows are what is drilled
    // into afterwards.
    run_summary_ = new QLabel(run_side);
    run_summary_->setObjectName(QStringLiteral("RunSummary"));
    run_layout->addWidget(run_summary_);

    auto* result_splitter = new QSplitter(Qt::Horizontal, run_side);
    run_targets_ = new QTableWidget(0, 3, result_splitter);
    run_targets_->setObjectName(QStringLiteral("RunTargets"));
    run_targets_->setHorizontalHeaderLabels(
        {QStringLiteral("Host"), QStringLiteral("Outcome"), QStringLiteral("Detail")});
    run_targets_->verticalHeader()->setVisible(false);
    run_targets_->setSelectionBehavior(QAbstractItemView::SelectRows);
    run_targets_->setSelectionMode(QAbstractItemView::SingleSelection);
    run_targets_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    run_targets_->horizontalHeader()->setStretchLastSection(true);
    // Without this the stylesheet repaints a selected row in
    // QPalette::HighlightedText *after* the outcome colour was set, so the one
    // row an operator clicked on is the one row whose outcome loses its colour
    // -- see theme.qss and lm_ui_render_tests.
    run_targets_->setItemDelegate(new lm::ui::KeepForegroundDelegate(run_targets_));
    result_splitter->addWidget(run_targets_);

    run_output_ = new QPlainTextEdit(result_splitter);
    run_output_->setObjectName(QStringLiteral("RunOutput"));
    run_output_->setReadOnly(true);
    // Fixed-width for the same reason the editor is: this is a console
    // transcript, and column alignment is often the whole of what it says.
    run_output_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    result_splitter->addWidget(run_output_);
    result_splitter->setStretchFactor(0, 2);
    result_splitter->setStretchFactor(1, 3);
    run_layout->addWidget(result_splitter, 1);
    vertical->addWidget(run_side);

    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 2);
    layout->addWidget(vertical, 1);

    connect(reset_button, &QPushButton::clicked, this,
            [this] { editor_->setPlainText(starter_template()); });
    connect(select_all_button, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < host_list_->count(); ++row) {
            host_list_->item(row)->setCheckState(Qt::Checked);
        }
    });
    connect(clear_button, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < host_list_->count(); ++row) {
            host_list_->item(row)->setCheckState(Qt::Unchecked);
        }
    });
    connect(host_list_, &QListWidget::itemChanged, this, [this](QListWidgetItem*) {
        if (!rebuilding_) {
            update_target_count();
        }
    });
    connect(run_button_, &QPushButton::clicked, this, &ScriptsTab::on_run_clicked);
    connect(run_targets_, &QTableWidget::itemSelectionChanged, this,
            &ScriptsTab::update_run_output);
    connect(controller_, &ServerController::fleet_changed, this, &ScriptsTab::rebuild_host_list);
    connect(controller_, &ServerController::script_run_changed, this,
            &ScriptsTab::on_script_run_changed);

    rebuild_host_list();
    refresh_run_view();
}

void ScriptsTab::rebuild_host_list() {
    // Ticks survive a rebuild: fleet_changed() fires whenever any host changes
    // state, and losing a hand-made selection to an unrelated machine going
    // offline mid-edit would be its own kind of blast radius.
    QSet<QString> previously_checked;
    for (int row = 0; row < host_list_->count(); ++row) {
        const QListWidgetItem* item = host_list_->item(row);
        if (item->checkState() == Qt::Checked) {
            previously_checked.insert(item->data(ScriptsTab::kHostIdRole).toString());
        }
    }

    rebuilding_ = true;
    host_list_->clear();
    for (const lm::core::FleetEntry& entry : controller_->fleet().entries) {
        const QString host_id = QString::fromStdString(entry.host_id);
        const QString note = note_for(entry);

        // Listed, explained and -- when it cannot comply -- unchecked, never
        // hidden. An operator should see that a machine is excluded and why,
        // not wonder where it went; and they may still tick it deliberately,
        // which is why the row stays checkable rather than being disabled.
        auto* item = new QListWidgetItem(
            note.isEmpty() ? host_id : host_id + QStringLiteral(" — ") + note,
            host_list_);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setData(ScriptsTab::kHostIdRole, host_id);
        // A tick survives even on a row that has since become ineligible: it
        // was a deliberate act, and silently undoing it would be worse than
        // letting the run record the refusal.
        item->setCheckState(previously_checked.contains(host_id) ? Qt::Checked : Qt::Unchecked);
    }
    rebuilding_ = false;

    update_target_count();
}

std::vector<std::string> ScriptsTab::checked_hosts() const {
    std::vector<std::string> hosts;
    for (int row = 0; row < host_list_->count(); ++row) {
        const QListWidgetItem* item = host_list_->item(row);
        if (item->checkState() == Qt::Checked) {
            hosts.push_back(item->data(ScriptsTab::kHostIdRole).toString().toStdString());
        }
    }
    return hosts;
}

void ScriptsTab::update_target_count() {
    const auto count = checked_hosts().size();
    // Run with nothing targeted is a no-op that reads as a failure. The button
    // says so by being unavailable rather than by doing nothing.
    run_button_->setEnabled(count > 0);
    if (count == 0) {
        target_count_label_->setText(QStringLiteral("No hosts selected"));
    } else if (count == 1) {
        target_count_label_->setText(QStringLiteral("Run on 1 host"));
    } else {
        target_count_label_->setText(
            QStringLiteral("Run on %1 hosts").arg(static_cast<int>(count)));
    }
}

void ScriptsTab::on_run_clicked() {
    const std::vector<std::string> hosts = checked_hosts();
    if (hosts.empty()) {
        return;  // the button is disabled; belt and braces for a programmatic click
    }
    const QString run_id = controller_->start_script_run(
        kCustomScriptName.toStdString(), editor_->toPlainText().toStdString(), hosts,
        kDefaultTimeoutSeconds);

    displayed_run_id_ = run_id.toStdString();
    // A different run is a different set of hosts, so its rows are built from
    // nothing rather than written over the previous run's -- a row updated in
    // place would keep the old run's selection while standing for another
    // machine.
    run_targets_->setRowCount(0);
    refresh_run_view();
    if (run_targets_->rowCount() > 0) {
        // Something in the output pane from the outset. On a one-host run that
        // is the whole answer, and on a large one it at least says what the
        // pane is for.
        run_targets_->selectRow(0);
    }
}

void ScriptsTab::on_script_run_changed(QString run_id) {
    if (run_id.toStdString() == displayed_run_id_) {
        refresh_run_view();
    }
}

const ScriptRun* ScriptsTab::displayed_run() const {
    if (displayed_run_id_.empty()) {
        return nullptr;
    }
    const std::vector<ScriptRun>& runs = controller_->script_runs();
    const auto found = std::ranges::find(runs, displayed_run_id_, &ScriptRun::run_id);
    return found == runs.end() ? nullptr : &*found;
}

void ScriptsTab::refresh_run_view() {
    const ScriptRun* run = displayed_run();
    if (run == nullptr) {
        run_summary_->setText(QStringLiteral("No run yet"));
        run_targets_->setRowCount(0);
        update_run_output();
        return;
    }

    const int rows = static_cast<int>(run->targets.size());
    if (run_targets_->rowCount() != rows) {
        run_targets_->setRowCount(rows);
    }
    const QColor text_colour(lm::ui::Theme::kText);
    const QColor muted(lm::ui::Theme::kTextMuted);
    for (int row = 0; row < rows; ++row) {
        const RunTarget& target = run->targets[static_cast<std::size_t>(row)];
        set_cell(run_targets_, row, 0, QString::fromStdString(target.host_id), text_colour);
        set_cell(run_targets_, row, 1, display_name(target.state), colour_for(target.state));
        set_cell(run_targets_, row, 2, detail_for(target), muted);
    }
    run_targets_->resizeColumnToContents(0);
    run_targets_->resizeColumnToContents(1);

    run_summary_->setText(summarise(run->tally()));
    update_run_output();
}

const RunTarget* ScriptsTab::selected_target() const {
    const ScriptRun* run = displayed_run();
    if (run == nullptr) {
        return nullptr;
    }
    const QModelIndexList selected = run_targets_->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        return nullptr;
    }
    const int row = selected.front().row();
    if (row < 0 || row >= static_cast<int>(run->targets.size())) {
        return nullptr;
    }
    // Rows are built in target order and never sorted, which is what lets a
    // row index stand for a target.
    return &run->targets[static_cast<std::size_t>(row)];
}

void ScriptsTab::update_run_output() {
    const RunTarget* target = selected_target();
    const QString text =
        target != nullptr
            ? output_for(*target)
            : (displayed_run() == nullptr
                   ? QStringLiteral("Press Run to send the script.")
                   : QStringLiteral("Select a host to see what it reported."));

    // Only when it actually changed. setPlainText() replaces the whole
    // document -- scrollbar to the top, cursor to the start, any selected text
    // gone -- and this runs on every script_run_changed. On the ninety-host run
    // this view exists for, somebody reading PC-007's transcript would be
    // yanked back to the top each time an unrelated host reported.
    if (run_output_->toPlainText() != text) {
        run_output_->setPlainText(text);
    }
}
