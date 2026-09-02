#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>

#include <QColor>
#include <QImage>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fleet_window.hpp"
#include "lm/ui/theme.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "script_run.hpp"
#include "scripts_tab.hpp"
#include "pixel_probe.hpp"
#include "server_controller.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

/// A window over a controller on an in-memory bus, with the bus reachable so a
/// test can play the part of a client and announce hosts into the fleet.
///
/// Every test file here defines its own harness rather than sharing one: the
/// shapes differ (this one needs a FleetWindow, test_script_dispatch.cpp needs
/// the dispatched commands), and a shared one would grow to be the union.
struct Harness {
    MessageBus bus;
    QTemporaryDir dir;
    std::unique_ptr<ServerController> controller;
    std::unique_ptr<FleetWindow> window;

    Harness() {
        EXPECT_TRUE(dir.isValid());
        controller = std::make_unique<ServerController>(make_in_memory_server(bus), dir.path());
        controller->start();
        window = std::make_unique<FleetWindow>(controller.get());
        window->resize(1100, 700);
        window->show();
        QApplication::processEvents();
    }

    ~Harness() { controller->stop(); }

    /// Brings a host into the fleet as Online, with the capabilities given.
    void announce(const std::string& host, Capabilities caps) {
        const auto client = make_in_memory_client(bus);
        ClientAnnounce message;
        message.host_id = host;
        message.agent_version = "test";
        message.capabilities = caps.raw();
        client->publish_announce(message);
        controller->add_expected_host(host, "");
        QApplication::processEvents();
    }

    /// Plays the part of a client answering a run. Takes the whole message so
    /// a case about *output* can set stdout_text, where a case about state
    /// only cares which ScriptStatus came back.
    void publish_result_message(const ScriptResultMessage& message) {
        const auto client = make_in_memory_client(bus);
        client->publish_script_result(message);
        QApplication::processEvents();
    }

    void publish_result(const std::string& host, const std::string& run_id, ScriptStatus status) {
        ScriptResultMessage message;
        message.host_id = host;
        message.run_id = run_id;
        message.status = status;
        publish_result_message(message);
    }
};

/// A machine that can run anything asked of it: enrolled, and running as an
/// account that can elevate. The default for cases that are not about a
/// capability, so their rows carry no suffix at all.
Capabilities enrolled() {
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Scripts).add(Capability::Elevated);
    return caps;
}

/// Enrolled, but the agent cannot elevate. Still perfectly able to run a
/// script that needs no admin, which is why this is a marker and not a
/// refusal.
Capabilities enrolled_unelevated() {
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Scripts);
    return caps;
}

/// Reporting, but never opted in to running scripts.
Capabilities not_enrolled() {
    Capabilities caps;
    caps.add(Capability::Resources);
    return caps;
}

QPlainTextEdit* scripts_editor(const Harness& harness) {
    return harness.window->findChild<QPlainTextEdit*>(QStringLiteral("ScriptEditor"));
}

QListWidget* host_list(const Harness& harness) {
    return harness.window->findChild<QListWidget*>(QStringLiteral("HostList"));
}

QPushButton* button(const Harness& harness, const QString& name) {
    QPushButton* found = harness.window->findChild<QPushButton*>(name);
    EXPECT_NE(found, nullptr) << name.toStdString();
    return found;
}

QLabel* label(const Harness& harness, const QString& name) {
    return harness.window->findChild<QLabel*>(name);
}

/// The host id a row stands for, read from the role the widget stores it in.
/// Not parsed back out of the text: the text carries a suffix, and the whole
/// point of the role is that nothing has to know what suffixes exist.
QString host_id_of(const QListWidgetItem* item) {
    return item->data(ScriptsTab::kHostIdRole).toString();
}

std::vector<QString> checked_hosts(const Harness& harness) {
    std::vector<QString> checked;
    QListWidget* hosts = host_list(harness);
    for (int row = 0; row < hosts->count(); ++row) {
        if (hosts->item(row)->checkState() == Qt::Checked) {
            checked.push_back(host_id_of(hosts->item(row)));
        }
    }
    return checked;
}

void check_hosts(const Harness& harness, const std::vector<std::string>& wanted) {
    QListWidget* hosts = host_list(harness);
    for (int row = 0; row < hosts->count(); ++row) {
        QListWidgetItem* item = hosts->item(row);
        const std::string id = host_id_of(item).toStdString();
        if (std::ranges::find(wanted, id) != wanted.end()) {
            item->setCheckState(Qt::Checked);
        }
    }
}

/// Clicks Run and hands back the id of the run it created. Taken from the
/// controller rather than read off the view, so a test that then asserts about
/// the view is not checking the view against itself.
QString press_run(const Harness& harness) {
    button(harness, QStringLiteral("RunButton"))->click();
    QApplication::processEvents();
    EXPECT_FALSE(harness.controller->script_runs().empty());
    return QString::fromStdString(harness.controller->script_runs().back().run_id);
}

/// The run table's row for a host. By id rather than by position, so no case
/// here silently depends on the order the fleet happens to be in.
int run_row_of(const Harness& harness, const QString& host_id);

/// Brings the Scripts tab to the front. Only a painting case needs this: a
/// widget on a background tab is never shown, so it is never polished against
/// the stylesheet and renders the unstyled fallback instead.
void show_scripts_tab(const Harness& harness) {
    auto* tabs = harness.window->findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == QStringLiteral("Scripts")) {
            tabs->setCurrentIndex(i);
        }
    }
    QApplication::processEvents();
}

QTableWidget* run_targets(const Harness& harness) {
    return harness.window->findChild<QTableWidget*>(QStringLiteral("RunTargets"));
}

QStringList outcome_column(const Harness& harness) {
    QStringList outcomes;
    QTableWidget* table = run_targets(harness);
    for (int row = 0; row < table->rowCount(); ++row) {
        outcomes << table->item(row, 1)->text();
    }
    return outcomes;
}

int run_row_of(const Harness& harness, const QString& host_id) {
    QTableWidget* table = run_targets(harness);
    for (int row = 0; row < table->rowCount(); ++row) {
        if (table->item(row, 0)->text() == host_id) {
            return row;
        }
    }
    return -1;
}

QListWidgetItem* row_for(const Harness& harness, const QString& host_id) {
    QListWidget* hosts = host_list(harness);
    for (int row = 0; row < hosts->count(); ++row) {
        if (host_id_of(hosts->item(row)) == host_id) {
            return hosts->item(row);
        }
    }
    return nullptr;
}

}  // namespace

TEST(ScriptsTab, OpensWithTheStarterTemplateRatherThanAnEmptyBox) {
    // The template is the only place the LM-RESULT convention is taught:
    // documentation nobody reads versus boilerplate that gets edited.
    Harness harness;
    auto* editor = scripts_editor(harness);
    ASSERT_NE(editor, nullptr);

    const QString text = editor->toPlainText();
    EXPECT_NE(text.indexOf(QStringLiteral("LM-RESULT")), -1) << text.toStdString();
    EXPECT_NE(text.indexOf(QStringLiteral("exit 0")), -1);
    EXPECT_NE(text.indexOf(QStringLiteral("exit 1")), -1)
        << "the failure path is the half people get wrong";
}

TEST(ScriptsTab, ListsEveryFleetHostWithACheckbox) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());

    QListWidget* hosts = host_list(harness);
    ASSERT_NE(hosts, nullptr);
    ASSERT_EQ(hosts->count(), 2);
    for (int i = 0; i < hosts->count(); ++i) {
        EXPECT_TRUE(hosts->item(i)->flags() & Qt::ItemIsUserCheckable);
    }
}

TEST(ScriptsTab, SelectAllAndClearMoveEveryCheckbox) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());

    button(harness, QStringLiteral("SelectAllButton"))->click();
    EXPECT_EQ(checked_hosts(harness).size(), 2);

    button(harness, QStringLiteral("ClearButton"))->click();
    EXPECT_EQ(checked_hosts(harness).size(), 0);
}

TEST(ScriptsTab, DisablesRunUntilAHostIsSelected) {
    // Run with nothing targeted is a no-op that looks like a failure. The
    // button says so by being unavailable rather than by doing nothing.
    Harness harness;
    harness.announce("PC-001", enrolled());

    EXPECT_FALSE(button(harness, QStringLiteral("RunButton"))->isEnabled());

    check_hosts(harness, {"PC-001"});
    EXPECT_TRUE(button(harness, QStringLiteral("RunButton"))->isEnabled());
}

TEST(ScriptsTab, ResetToTemplateRestoresTheStarter) {
    Harness harness;
    QPlainTextEdit* editor = scripts_editor(harness);
    const QString original = editor->toPlainText();

    editor->setPlainText(QStringLiteral("Remove-Item C:\\ -Recurse"));
    button(harness, QStringLiteral("ResetTemplateButton"))->click();

    EXPECT_EQ(editor->toPlainText(), original);
}

TEST(ScriptsTab, StartsARunWithTheEditorsBodyAndTheCheckedHosts) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    scripts_editor(harness)->setPlainText(QStringLiteral("Write-Output 'hi'\nexit 0\n"));
    check_hosts(harness, {"PC-002"});

    button(harness, QStringLiteral("RunButton"))->click();
    QApplication::processEvents();

    ASSERT_EQ(harness.controller->script_runs().size(), 1u);
    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(run.script_body, "Write-Output 'hi'\nexit 0\n");
    ASSERT_EQ(run.targets.size(), 1u) << "only the checked host is targeted";
    EXPECT_EQ(run.targets.front().host_id, "PC-002");
}

TEST(ScriptsTab, ListsAHostThatCannotComplyExplainedAndUnchecked) {
    // Listed, not hidden: an operator should see that a machine is excluded
    // and why, rather than wonder where it went. Select all leaves it alone,
    // so no bulk gesture ever targets a machine that cannot run the script --
    // but a deliberate tick still does, which is the operator's call.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-003", not_enrolled());

    QListWidgetItem* refused = row_for(harness, QStringLiteral("PC-003"));
    ASSERT_NE(refused, nullptr);
    EXPECT_NE(refused->text().indexOf(QStringLiteral("not enrolled")), -1)
        << refused->text().toStdString();
    EXPECT_EQ(refused->checkState(), Qt::Unchecked);

    button(harness, QStringLiteral("SelectAllButton"))->click();
    EXPECT_EQ(row_for(harness, QStringLiteral("PC-003"))->checkState(), Qt::Unchecked);
    EXPECT_EQ(row_for(harness, QStringLiteral("PC-001"))->checkState(), Qt::Checked);

    refused->setCheckState(Qt::Checked);
    EXPECT_EQ(checked_hosts(harness).size(), 2) << "the operator may still override";
}

TEST(ScriptsTab, MarksAHostThatCannotElevateWithoutExcludingIt) {
    // Marked, not refused. Phase 1 cannot ask for elevation and plenty of
    // scripts need no admin, so this machine runs them -- the flag is here so
    // an access-denied is read before the run rather than as a column of
    // failures afterwards.
    Harness harness;
    harness.announce("PC-004", enrolled_unelevated());

    QListWidgetItem* row = row_for(harness, QStringLiteral("PC-004"));
    ASSERT_NE(row, nullptr);
    EXPECT_NE(row->text().indexOf(QStringLiteral("not elevated")), -1)
        << row->text().toStdString();

    button(harness, QStringLiteral("SelectAllButton"))->click();
    EXPECT_EQ(row_for(harness, QStringLiteral("PC-004"))->checkState(), Qt::Checked)
        << "a marker is not a refusal: the sweep still takes this host";

    button(harness, QStringLiteral("RunButton"))->click();
    QApplication::processEvents();
    ASSERT_EQ(harness.controller->script_runs().size(), 1u);
    const ScriptRun& run = harness.controller->script_runs().back();
    ASSERT_EQ(run.targets.size(), 1u);
    EXPECT_EQ(run.targets.front().state, TargetState::Dispatched)
        << "and the script really goes out to it";
}

TEST(ScriptsTab, NamesTheStateOfAHostThatIsNotReporting) {
    // Expected, never heard from. The row says which of the four ways it is
    // absent, because Offline, Missing and Unexpected send the reader
    // somewhere different.
    Harness harness;
    harness.controller->add_expected_host("PC-009", "");
    QApplication::processEvents();

    QListWidgetItem* row = row_for(harness, QStringLiteral("PC-009"));
    ASSERT_NE(row, nullptr);
    EXPECT_NE(row->text().indexOf(QStringLiteral("Missing")), -1) << row->text().toStdString();
    EXPECT_EQ(row->checkState(), Qt::Unchecked);

    button(harness, QStringLiteral("SelectAllButton"))->click();
    EXPECT_EQ(row_for(harness, QStringLiteral("PC-009"))->checkState(), Qt::Unchecked)
        << "nothing can be dispatched to it, so the sweep leaves it alone";
}

TEST(ScriptsTab, RefreshesARowWhenOnlyItsCapabilitiesChange) {
    // A liveliness drop erases the registry entry; the resource samples still
    // arriving recreate it with no capabilities, so the host reads Online and
    // un-enrolled. The next announce restores them at the same Online state --
    // and until ServerController::reconcile_now() compared capabilities as
    // well as state, that emitted nothing and this row went on saying "not
    // enrolled" until some unrelated machine happened to change state.
    Harness harness;
    harness.announce("PC-001", not_enrolled());
    ASSERT_NE(row_for(harness, QStringLiteral("PC-001")), nullptr);
    ASSERT_NE(row_for(harness, QStringLiteral("PC-001"))->text().indexOf(
                  QStringLiteral("not enrolled")),
              -1);

    harness.announce("PC-001", enrolled());

    EXPECT_EQ(row_for(harness, QStringLiteral("PC-001"))->text(), QStringLiteral("PC-001"))
        << "the row has to follow the capability, not wait for a state change";
}

TEST(ScriptsTab, SaysHowManyHostsRunWillReach) {
    // The blast radius must be readable without counting checkboxes.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());

    QLabel* count = label(harness, QStringLiteral("TargetCountLabel"));
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->text(), QStringLiteral("No hosts selected"));

    check_hosts(harness, {"PC-001"});
    EXPECT_EQ(count->text(), QStringLiteral("Run on 1 host"));

    button(harness, QStringLiteral("SelectAllButton"))->click();
    EXPECT_EQ(count->text(), QStringLiteral("Run on 2 hosts"));
}

TEST(ScriptRunView, ShowsEveryTargetAsPendingTheMomentRunIsPressed) {
    // Somebody who just dispatched to ninety machines needs to see that it
    // started -- not a blank pane until the first result lands.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});

    press_run(harness);

    ASSERT_NE(run_targets(harness), nullptr);
    ASSERT_EQ(run_targets(harness)->rowCount(), 2);
    for (const QString& outcome : outcome_column(harness)) {
        EXPECT_EQ(outcome.toStdString(), "Dispatched");
    }
}

TEST(ScriptRunView, MovesATargetInPlaceWhenItsResultArrives) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});
    const QString run_id = press_run(harness);

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    // The whole ordered list, not containment: a rebuilt, reordered or
    // wholesale-replaced table would satisfy containment, and not replacing
    // rows is the one thing set_cell() exists for.
    const QStringList outcomes = outcome_column(harness);
    EXPECT_EQ(outcomes, (QStringList{QStringLiteral("Completed"), QStringLiteral("Dispatched")}))
        << "the host that has not answered must not move, and neither must its row";
}

TEST(ScriptRunView, SummarisesTheRunWithACountPerOutcome) {
    // On a large run the tally is what is read; the rows are what is drilled
    // into afterwards.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});
    const QString run_id = press_run(harness);

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    harness.publish_result("PC-002", run_id.toStdString(), ScriptStatus::Failed);

    auto* summary = harness.window->findChild<QLabel*>(QStringLiteral("RunSummary"));
    ASSERT_NE(summary, nullptr);
    const std::string text = summary->text().toStdString();
    EXPECT_NE(text.find("1 completed"), std::string::npos) << text;
    EXPECT_NE(text.find("1 failed"), std::string::npos) << text;
}

TEST(ScriptRunView, ShowsTheOutputOfTheSelectedTarget) {
    // Two hosts, and the output of the one the run did *not* auto-select:
    // pressing Run already selects row 0, so a case that then selects row 0
    // would pass with the selection-changed connection deleted.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});
    const QString run_id = press_run(harness);

    ScriptResultMessage result;
    result.host_id = "PC-002";
    result.run_id = run_id.toStdString();
    result.status = ScriptStatus::Completed;
    result.stdout_text = "cleaned 3 files";
    harness.publish_result_message(result);

    const int row = run_row_of(harness, QStringLiteral("PC-002"));
    ASSERT_GT(row, 0) << "PC-002 must not be the row Run already selected";
    run_targets(harness)->selectRow(row);
    QApplication::processEvents();

    auto* output = harness.window->findChild<QPlainTextEdit*>(QStringLiteral("RunOutput"));
    ASSERT_NE(output, nullptr);
    EXPECT_NE(output->toPlainText().indexOf(QStringLiteral("cleaned 3 files")), -1)
        << output->toPlainText().toStdString();
}

TEST(ScriptRunView, ShowsARefusalsReasonRatherThanItsOutput) {
    // A refused host has no output; the reason is the whole of what happened,
    // and an empty pane would read as "ran and printed nothing".
    Harness harness;
    Capabilities bare;
    bare.add(Capability::Resources);
    harness.announce("PC-001", bare);
    check_hosts(harness, {"PC-001"});
    press_run(harness);

    run_targets(harness)->selectRow(0);
    QApplication::processEvents();

    auto* output = harness.window->findChild<QPlainTextEdit*>(QStringLiteral("RunOutput"));
    ASSERT_NE(output, nullptr);
    EXPECT_NE(output->toPlainText().indexOf(QStringLiteral("enrol")), -1)
        << output->toPlainText().toStdString();
}

TEST(ScriptRunView, KeepsTheSelectedRowWhenAnotherHostReports) {
    // The pane below the table is somebody reading one machine's transcript.
    // A result for a different machine must not move them off it.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});
    const QString run_id = press_run(harness);

    QTableWidget* table = run_targets(harness);
    table->selectRow(1);
    QApplication::processEvents();

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    const QModelIndexList selected = table->selectionModel()->selectedRows();
    ASSERT_EQ(selected.size(), 1);
    EXPECT_EQ(selected.front().row(), 1)
        << "a result for another host moved the selection";
}

TEST(ScriptRunView, PaintsEachOutcomeInThePalettesColourForIt) {
    // Named colours, so the run view and the fleet table cannot drift into
    // disagreeing about what red means. Dispatched deliberately does not share
    // NoResponse's grey: kTextMuted and kNotApplicable are the same value, and
    // "still waiting" must not look like "gave up waiting".
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    harness.announce("PC-003", enrolled());
    harness.announce("PC-004", not_enrolled());
    check_hosts(harness, {"PC-001", "PC-002", "PC-003", "PC-004"});
    const QString run_id = press_run(harness);

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    harness.publish_result("PC-002", run_id.toStdString(), ScriptStatus::Failed);

    QTableWidget* table = run_targets(harness);
    const auto outcome_colour = [&](const QString& host) {
        return table->item(run_row_of(harness, host), 1)->foreground().color();
    };
    EXPECT_EQ(outcome_colour(QStringLiteral("PC-001")), QColor(lm::ui::Theme::kOnline));
    EXPECT_EQ(outcome_colour(QStringLiteral("PC-002")), QColor(lm::ui::Theme::kMissing));
    EXPECT_EQ(outcome_colour(QStringLiteral("PC-003")), QColor(lm::ui::Theme::kPaused));
    EXPECT_EQ(outcome_colour(QStringLiteral("PC-004")), QColor(lm::ui::Theme::kOffline));
}

TEST(ScriptRunView, KeepsAnOutcomeColourOnTheRowThatIsSelected) {
    // QSS beats an item delegate and does it last: with a stylesheet active,
    // QStyleSheetStyle hands the ::item rule's own colour in as
    // HighlightedText *after* the delegate has run, so the one row an operator
    // clicked on is the one row whose outcome loses its colour. Only painting
    // can see that -- it is invisible to every logical assertion above, and
    // this trap has now caught three widgets in this codebase.
    Harness harness;
    show_scripts_tab(harness);
    harness.announce("PC-001", enrolled());
    check_hosts(harness, {"PC-001"});
    const QString run_id = press_run(harness);
    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    QTableWidget* table = run_targets(harness);
    table->selectRow(0);
    QApplication::processEvents();

    // The Outcome cell alone. The Host cell beside it is painted kText on
    // purpose, which would make the negative assertion below meaningless over
    // the whole viewport.
    const QRect cell = table->visualRect(table->model()->index(0, 1));
    ASSERT_FALSE(cell.isEmpty());
    const QImage painted = lm::ui::test::paint(*table->viewport()).copy(cell);

    EXPECT_TRUE(lm::ui::test::contains_colour(painted, QColor(lm::ui::Theme::kOnline)))
        << "the selected row lost the outcome colour it carries when unselected";
    EXPECT_FALSE(lm::ui::test::contains_colour(painted, QColor(lm::ui::Theme::kText)))
        << "something repainted the selected outcome in kText, discarding its colour";
}

TEST(ScriptRunView, CountsOnlyTheOutcomesThatHappened) {
    // The whole string. "1 completed" is a substring of
    // "0 pending · 0 dispatched · 1 completed", and four zeroes beside the one
    // number that matters is exactly what this omits.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});
    const QString run_id = press_run(harness);

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    harness.publish_result("PC-002", run_id.toStdString(), ScriptStatus::Failed);

    auto* summary = harness.window->findChild<QLabel*>(QStringLiteral("RunSummary"));
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->text().toStdString(), std::string("1 completed · 1 failed"));
}

TEST(ScriptRunView, IgnoresAResultForARunItIsNotShowing) {
    // script_run_changed fires for every result of every run this server has
    // issued. Repainting the view on one that is not on screen would show the
    // operator numbers belonging to a different dispatch.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    check_hosts(harness, {"PC-001", "PC-002"});
    const QString first_run = press_run(harness);
    const QString second_run = press_run(harness);
    ASSERT_NE(first_run, second_run);

    auto* summary = harness.window->findChild<QLabel*>(QStringLiteral("RunSummary"));
    const QString before = summary->text();

    harness.publish_result("PC-001", first_run.toStdString(), ScriptStatus::Completed);

    EXPECT_EQ(summary->text().toStdString(), before.toStdString());
    for (const QString& outcome : outcome_column(harness)) {
        EXPECT_EQ(outcome.toStdString(), "Dispatched")
            << "the displayed run moved because an older one did";
    }
}
