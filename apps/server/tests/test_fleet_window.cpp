#include <gtest/gtest.h>

#include <QApplication>
#include <QCompleter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <memory>

#include "add_rule_dialog.hpp"
#include "fleet_window.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "lm/ui/token_edit.hpp"
#include "server_controller.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

/// A window over a controller with a draft holding two templates and one
/// assignment, which is the state every case here starts from. Owns the bus so
/// it outlives the controller that references it.
struct Harness {
    MessageBus bus;
    QTemporaryDir dir;
    std::unique_ptr<ServerController> controller;
    std::unique_ptr<FleetWindow> window;

    Harness() {
        EXPECT_TRUE(dir.isValid());
        controller = std::make_unique<ServerController>(make_in_memory_server(bus), dir.path());

        Template workstation;
        workstation.name = "Lab Workstation";
        Template build_server;
        build_server.name = "Build Server";
        controller->draft().templates.push_back(workstation);
        controller->draft().templates.push_back(build_server);
        controller->draft().assignments["PC-001"] = {"Lab Workstation"};

        window = std::make_unique<FleetWindow>(controller.get());
        window->resize(1100, 700);
        window->show();
        QApplication::processEvents();
    }

    ~Harness() { controller->stop(); }

    /// The Templates tab's assignment table is the only QTableWidget with a
    /// TokenEdit in it, which is a more durable handle than an object name.
    [[nodiscard]] lm::ui::TokenEdit* assignment_editor(int row = 0) const {
        for (QTableWidget* table : window->findChildren<QTableWidget*>()) {
            if (auto* editor = qobject_cast<lm::ui::TokenEdit*>(table->cellWidget(row, 1))) {
                return editor;
            }
        }
        return nullptr;
    }

    /// The Templates tab's template list — the only QListWidget in the window.
    [[nodiscard]] QListWidget* template_list() const {
        const QList<QListWidget*> lists = window->findChildren<QListWidget*>();
        return lists.isEmpty() ? nullptr : lists.first();
    }

    [[nodiscard]] QPushButton* button(const QString& text) const {
        for (QPushButton* candidate : window->findChildren<QPushButton*>()) {
            if (candidate->text() == text) {
                return candidate;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t template_count() const {
        return controller->draft().templates.size();
    }
};

/// Types a name into an assignment editor and commits it the way a user does.
void type_and_commit(lm::ui::TokenEdit* editor, const QString& text) {
    QLineEdit* input = editor->findChild<QLineEdit*>();
    ASSERT_NE(input, nullptr);
    input->setFocus();
    QTest::keyClicks(input, text);
    QTest::keyClick(input, Qt::Key_Return);
    QApplication::processEvents();
}

}  // namespace

namespace {

/// The compliance tab's tree — the only one headed "Host / rule".
QTreeWidget* compliance_tree(const Harness& harness) {
    for (QTreeWidget* tree : harness.window->findChildren<QTreeWidget*>()) {
        if (tree->headerItem() != nullptr &&
            tree->headerItem()->text(0) == QStringLiteral("Host / rule")) {
            return tree;
        }
    }
    return nullptr;
}

/// Every row under a host, as "<text col 0>|<text col 1>".
QStringList rows_under(QTreeWidgetItem* host) {
    QStringList rows;
    for (int i = 0; i < host->childCount(); ++i) {
        rows << host->child(i)->text(0) + QStringLiteral("|") + host->child(i)->text(1);
    }
    return rows;
}

/// The compliance tab's headline, found by what it says rather than by an
/// object name — it is the only label in the window that scores the fleet.
QString compliance_headline(const Harness& harness) {
    for (QLabel* label : harness.window->findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("fully compliant")) ||
            label->text().contains(QStringLiteral("No host has reported"))) {
            return label->text();
        }
    }
    return {};
}

ComplianceReport report_for(const std::string& host, std::vector<CheckStatus> statuses) {
    ComplianceReport report;
    report.host_id = host;
    report.applied_revision = 4;
    int n = 0;
    for (const CheckStatus status : statuses) {
        report.results.push_back(CheckResult{"r" + std::to_string(n++), status, "", ""});
    }
    return report;
}

/// Publishes a report the way a client does, rather than emitting the
/// controller's signal directly: the table is built from the controller's
/// report cache, which only the real receive path fills.
void publish_report(Harness& harness, const ComplianceReport& report) {
    const auto client = make_in_memory_client(harness.bus);
    ComplianceReportMessage message;
    message.report = report;
    client->publish_report(message);
    QApplication::processEvents();
}

}  // namespace

TEST(FleetWindowCompliance, ScoresEachHostAsPassedOverChecked) {
    Harness harness;
    ASSERT_NE(compliance_tree(harness), nullptr) << "no compliance tab";

    harness.controller->start();
    publish_report(harness,
                   report_for("PC-001", {CheckStatus::Pass, CheckStatus::Pass, CheckStatus::Fail}));

    QTreeWidget* tree = compliance_tree(harness);
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_EQ(tree->topLevelItem(0)->text(0).toStdString(), "PC-001");
    EXPECT_EQ(tree->topLevelItem(0)->text(1).toStdString(), "2 / 3 rules passed");
}

TEST(FleetWindowCompliance, ShowsEveryFailingRuleWithWhatWasObserved) {
    // The whole point on a display nobody can click: what is wrong, and why,
    // has to be on the glass already.
    Harness harness;
    harness.controller->start();

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"r1", CheckStatus::Pass, "running", ""},
                      CheckResult{"r2", CheckStatus::Fail, "No link", ""},
                      CheckResult{"r3", CheckStatus::Error, "read failed", "ACCESS_DENIED"}};
    publish_report(harness, report);

    QTreeWidgetItem* host = compliance_tree(harness)->topLevelItem(0);
    const QStringList rows = rows_under(host);
    ASSERT_EQ(rows.size(), 2) << "passing rules are counted, not listed";
    EXPECT_TRUE(rows[0].contains(QStringLiteral("r2"))) << rows[0].toStdString();
    EXPECT_TRUE(rows[0].contains(QStringLiteral("No link")));
    EXPECT_TRUE(rows[1].contains(QStringLiteral("ACCESS_DENIED")))
        << "an error's message is the only thing that explains it";
}

TEST(FleetWindowCompliance, PutsFailuresAboveErrorsAndNotApplicable) {
    Harness harness;
    harness.controller->start();

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"na", CheckStatus::NotApplicable, "no capability", ""},
                      CheckResult{"err", CheckStatus::Error, "read failed", ""},
                      CheckResult{"bad", CheckStatus::Fail, "not running", ""}};
    publish_report(harness, report);

    const QStringList rows = rows_under(compliance_tree(harness)->topLevelItem(0));
    ASSERT_EQ(rows.size(), 3);
    EXPECT_TRUE(rows[0].contains(QStringLiteral("bad")));
    EXPECT_TRUE(rows[1].contains(QStringLiteral("err")));
    EXPECT_TRUE(rows[2].contains(QStringLiteral("na")));
}

TEST(FleetWindowCompliance, SaysSoExplicitlyWhenAHostIsFullyCompliant) {
    // An empty group reads as "no data" from across a room.
    Harness harness;
    harness.controller->start();
    publish_report(harness, report_for("PC-001", {CheckStatus::Pass, CheckStatus::Pass}));

    const QStringList rows = rows_under(compliance_tree(harness)->topLevelItem(0));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_TRUE(rows.first().contains(QStringLiteral("All 2 checked rules passing")))
        << rows.first().toStdString();
}

TEST(FleetWindowCompliance, LeavesNotApplicableOutOfTheRatioButStillListsIt) {
    // A rule the client cannot evaluate can never pass, so counting it in the
    // denominator would park the host at a score it can never improve — but it
    // still has to be visible, or the rule looks silently satisfied.
    Harness harness;
    harness.controller->start();
    publish_report(harness, report_for("PC-001", {CheckStatus::Pass, CheckStatus::NotApplicable,
                                                   CheckStatus::NotApplicable}));

    QTreeWidgetItem* host = compliance_tree(harness)->topLevelItem(0);
    EXPECT_EQ(host->text(1).toStdString(), "1 / 1 rules passed");
    EXPECT_EQ(rows_under(host).size(), 2) << "the excluded rules must stay on screen";
}

TEST(FleetWindowCompliance, EveryGroupIsExpandedBecauseNobodyCanClickIt) {
    Harness harness;
    harness.controller->start();
    publish_report(harness, report_for("PC-001", {CheckStatus::Fail}));

    EXPECT_TRUE(compliance_tree(harness)->topLevelItem(0)->isExpanded());
}

TEST(FleetWindowCompliance, ListsTheWorstHostsFirst) {
    // The tab exists to find what needs attention; alphabetical order would
    // bury a host with three failures under one with none.
    Harness harness;

    harness.controller->start();
    publish_report(harness, report_for("PC-aaa", {CheckStatus::Pass}));
    publish_report(harness, report_for("PC-zzz", {CheckStatus::Fail, CheckStatus::Fail}));

    QTreeWidget* tree = compliance_tree(harness);
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    EXPECT_EQ(tree->topLevelItem(0)->text(0).toStdString(), "PC-zzz");
    EXPECT_EQ(tree->topLevelItem(1)->text(0).toStdString(), "PC-aaa");
}

TEST(FleetWindowCompliance, StartsEmptyBeforeAnyHostReports) {
    Harness harness;
    EXPECT_EQ(compliance_tree(harness)->topLevelItemCount(), 0);
}

TEST(FleetWindowCompliance, ListsAnExpectedHostThatHasNeverReported) {
    // A machine nobody can hear from cannot be compliant, and a tab that simply
    // leaves it out says, on a wall display, that nothing is wrong with it.
    Harness harness;
    harness.controller->start();
    harness.controller->add_expected_host("PC-dead", "10.0.0.9");

    QTreeWidget* tree = compliance_tree(harness);
    ASSERT_EQ(tree->topLevelItemCount(), 1);
    EXPECT_EQ(tree->topLevelItem(0)->text(0).toStdString(), "PC-dead");
    EXPECT_TRUE(tree->topLevelItem(0)->text(1).contains(QStringLiteral("Missing")))
        << tree->topLevelItem(0)->text(1).toStdString();
}

TEST(FleetWindowCompliance, SaysNoRulesAreBeingCheckedOnASilentHost) {
    // Not "0 / 0 rules passed", which reads as a clean bill of health.
    Harness harness;
    harness.controller->start();
    harness.controller->add_expected_host("PC-dead", "");

    QTreeWidgetItem* host = compliance_tree(harness)->topLevelItem(0);
    ASSERT_NE(host, nullptr);
    EXPECT_FALSE(host->text(1).contains(QStringLiteral("rules passed"))) << "a silent host has no score";

    const QStringList rows = rows_under(host);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_TRUE(rows.first().contains(QStringLiteral("no rules are being checked")))
        << rows.first().toStdString();
}

TEST(FleetWindowCompliance, PutsSilentHostsAboveFailingOnes) {
    // The fleet tab already ranks Missing above everything else: a machine that
    // is not answering is a bigger unknown than a rule known to be broken.
    Harness harness;
    harness.controller->start();
    publish_report(harness, report_for("PC-loud", {CheckStatus::Fail, CheckStatus::Fail}));
    harness.controller->add_expected_host("PC-dead", "");

    QTreeWidget* tree = compliance_tree(harness);
    ASSERT_EQ(tree->topLevelItemCount(), 2);
    EXPECT_EQ(tree->topLevelItem(0)->text(0).toStdString(), "PC-dead");
    EXPECT_EQ(tree->topLevelItem(1)->text(0).toStdString(), "PC-loud");
}

TEST(FleetWindowCompliance, CountsSilentHostsInTheHeadline) {
    Harness harness;
    harness.controller->start();
    publish_report(harness, report_for("PC-loud", {CheckStatus::Pass}));
    harness.controller->add_expected_host("PC-dead", "");

    const QString headline = compliance_headline(harness);
    EXPECT_TRUE(headline.contains(QStringLiteral("1 not reporting"))) << headline.toStdString();
}

TEST(FleetWindowAssignments, CreatesATemplateNamedInAnAssignment) {
    Harness harness;
    lm::ui::TokenEdit* editor = harness.assignment_editor();
    ASSERT_NE(editor, nullptr);

    type_and_commit(editor, QStringLiteral("Renderfarm"));

    EXPECT_EQ(editor->tokens().join(QStringLiteral("|")).toStdString(), "Lab Workstation|Renderfarm");
    EXPECT_EQ(harness.controller->draft().templates.size(), 3u);
    EXPECT_EQ(harness.controller->draft().assignments.at("PC-001").size(), 2u);
}

TEST(FleetWindowAssignments, CommitsAndClearsWhenFocusLeavesTheField) {
    Harness harness;
    lm::ui::TokenEdit* editor = harness.assignment_editor();
    ASSERT_NE(editor, nullptr);
    QLineEdit* input = editor->findChild<QLineEdit*>();

    input->setFocus();
    QTest::keyClicks(input, QStringLiteral("Renderfarm"));
    input->clearFocus();
    QApplication::processEvents();

    EXPECT_EQ(editor->tokens().join(QStringLiteral("|")).toStdString(), "Lab Workstation|Renderfarm");
    EXPECT_TRUE(input->text().isEmpty()) << "left in the input: " << input->text().toStdString();
}

TEST(FleetWindowAssignments, CommitsWhatWasTypedNotWhatTheDropdownSuggested) {
    // "Build" is a prefix of the existing "Build Server", so the dropdown is
    // open with that highlighted when Return arrives. A new template called
    // "Build" is a perfectly reasonable thing to want.
    Harness harness;
    lm::ui::TokenEdit* editor = harness.assignment_editor();
    ASSERT_NE(editor, nullptr);

    type_and_commit(editor, QStringLiteral("Build"));

    QLineEdit* input = editor->findChild<QLineEdit*>();
    EXPECT_EQ(editor->tokens().join(QStringLiteral("|")).toStdString(), "Lab Workstation|Build");
    EXPECT_TRUE(input->text().isEmpty()) << "left in the input: " << input->text().toStdString();
}

TEST(FleetWindowAssignments, LeavesNothingBehindInTheInputAfterCommitting) {
    Harness harness;
    lm::ui::TokenEdit* editor = harness.assignment_editor();
    ASSERT_NE(editor, nullptr);

    type_and_commit(editor, QStringLiteral("Renderfarm"));

    QLineEdit* input = editor->findChild<QLineEdit*>();
    EXPECT_TRUE(input->text().isEmpty())
        << "the committed name is still sitting in the input: " << input->text().toStdString();
}

TEST(FleetWindowBaseline, RefusesToAssignTheBaselineOrCreateATemplateForIt) {
    // This is what produced two "Baseline" rows: the name was unknown as a
    // template, so committing it created one alongside the bundle's baseline.
    Harness harness;
    lm::ui::TokenEdit* editor = harness.assignment_editor();
    ASSERT_NE(editor, nullptr);

    type_and_commit(editor, QStringLiteral("Baseline"));

    EXPECT_EQ(editor->tokens().join(QStringLiteral("|")).toStdString(), "Lab Workstation")
        << "the chip must not stick";
    EXPECT_EQ(harness.template_count(), 2u) << "no template may be created for the baseline";
    EXPECT_EQ(harness.controller->draft().assignments.at("PC-001").size(), 1u);
}

TEST(FleetWindowBaseline, RefusesItWhateverTheCapitalisation) {
    Harness harness;
    lm::ui::TokenEdit* editor = harness.assignment_editor();

    type_and_commit(editor, QStringLiteral("baseline"));

    EXPECT_EQ(harness.template_count(), 2u);
}

TEST(FleetWindowBaseline, DoesNotOfferTheBaselineAsACompletion) {
    Harness harness;
    lm::ui::TokenEdit* editor = harness.assignment_editor();
    ASSERT_NE(editor, nullptr);

    // On the widget, not via QLineEdit::completer() -- see token_edit.cpp for
    // why the completer is attached with setWidget() instead.
    QCompleter* completer = editor->findChild<QCompleter*>();
    ASSERT_NE(completer, nullptr);
    for (int row = 0; row < completer->model()->rowCount(); ++row) {
        EXPECT_NE(completer->model()->index(row, 0).data().toString(), QStringLiteral("Baseline"));
    }
}

TEST(FleetWindowBaseline, MarksTheBaselineRowAndDisablesRemovingIt) {
    Harness harness;
    QListWidget* list = harness.template_list();
    ASSERT_NE(list, nullptr);
    ASSERT_GT(list->count(), 0);

    list->setCurrentRow(0);
    QApplication::processEvents();

    EXPECT_TRUE(list->item(0)->text().contains(QStringLiteral("always applied")))
        << "the row has to say why it cannot be removed";
    EXPECT_TRUE(list->item(0)->font().italic());

    QPushButton* remove = harness.button(QStringLiteral("Remove Template"));
    ASSERT_NE(remove, nullptr);
    EXPECT_FALSE(remove->isEnabled()) << "greyed out beats silently doing nothing";
}

TEST(FleetWindowBaseline, EnablesRemovingAnOrdinaryTemplate) {
    Harness harness;
    QListWidget* list = harness.template_list();
    list->setCurrentRow(1);  // "Lab Workstation"
    QApplication::processEvents();

    EXPECT_TRUE(harness.button(QStringLiteral("Remove Template"))->isEnabled());
}

TEST(FleetWindowBaseline, LetsAStrayTemplateNamedBaselineBeRemoved) {
    // Bundles written by older builds hold one of these. The list shows it
    // next to the real baseline, and it has to be selectable and removable --
    // matching rows by their label made it neither.
    Harness harness;
    lm::core::Template stray;
    stray.name = "Baseline";
    harness.controller->draft().templates.push_back(stray);

    QListWidget* list = harness.template_list();
    harness.window->findChild<QListWidget*>();
    // Rebuild the way any edit does, then select the stray row (last).
    harness.controller->draft().assignments["PC-002"] = {};
    list->setCurrentRow(0);
    QApplication::processEvents();

    // Re-enter through the public path so the list is rebuilt from the draft.
    lm::ui::TokenEdit* editor = harness.assignment_editor();
    ASSERT_NE(editor, nullptr);
    type_and_commit(editor, QStringLiteral("Renderfarm"));

    ASSERT_EQ(list->count(), 5) << "baseline row + 4 templates";
    int stray_row = -1;
    for (int row = 1; row < list->count(); ++row) {
        if (list->item(row)->text() == QStringLiteral("Baseline")) {
            stray_row = row;
        }
    }
    ASSERT_NE(stray_row, -1) << "the stray template must still be listed, under its own name";

    list->setCurrentRow(stray_row);
    QApplication::processEvents();
    QPushButton* remove = harness.button(QStringLiteral("Remove Template"));
    ASSERT_TRUE(remove->isEnabled()) << "the stray one is an ordinary template and must be removable";
    remove->click();
    QApplication::processEvents();

    EXPECT_TRUE(std::ranges::none_of(harness.controller->draft().templates,
                                      [](const lm::core::Template& t) { return t.name == "Baseline"; }));
}

namespace {

/// The Templates tab's rule table -- the only QTableWidget headed
/// "Description". The assignment table beside it is headed "Host".
QTableWidget* rule_table(const Harness& harness) {
    for (QTableWidget* table : harness.window->findChildren<QTableWidget*>()) {
        if (table->horizontalHeaderItem(0) != nullptr &&
            table->horizontalHeaderItem(0)->text() == QStringLiteral("Description")) {
            return table;
        }
    }
    return nullptr;
}

/// Selects the template a rule was put into, since the rule table only ever
/// shows the selected one.
void select_template(const Harness& harness, const QString& name) {
    QListWidget* list = harness.template_list();
    ASSERT_NE(list, nullptr);
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->text().startsWith(name)) {
            list->setCurrentRow(i);
            QApplication::processEvents();
            return;
        }
    }
    FAIL() << "no template row named " << name.toStdString();
}

}  // namespace

TEST(FleetWindowRules, ShowsADdsTopicRuleAsSomethingAnOperatorCanRead) {
    Harness harness;
    Rule rule;
    rule.id = "dds-basket";
    rule.payload = DdsTopicRule{42, "Basket"};
    harness.controller->draft().templates.front().rules.push_back(rule);

    select_template(harness, QStringLiteral("Lab Workstation"));
    QTableWidget* table = rule_table(harness);
    ASSERT_NE(table, nullptr) << "no rule table";
    ASSERT_EQ(table->rowCount(), 1);

    EXPECT_EQ(table->item(0, 1)->text().toStdString(), "DDS") << "the kind column must name the kind";
    // Domain and topic together: the same topic name on two domains is two
    // different things, and the domain is the half a reader will not guess.
    EXPECT_EQ(table->item(0, 3)->text().toStdString(), "Basket on domain 42");
    // Presence *does* apply to a topic rule, so here it still shows.
    EXPECT_EQ(table->item(0, 2)->text().toStdString(), "Must be present");
}

TEST(FleetWindowRules, ShowsADdsValueRuleWithItsPathAndExpectation) {
    Harness harness;
    Rule rule;
    rule.id = "dds-basket-items-length";
    rule.payload = DdsValueRule{42, "Basket", "items_.length", DdsMatch::Equals, "2"};
    harness.controller->draft().templates.front().rules.push_back(rule);

    select_template(harness, QStringLiteral("Lab Workstation"));
    QTableWidget* table = rule_table(harness);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 1);

    const std::string target = table->item(0, 3)->text().toStdString();
    EXPECT_EQ(target, "Basket.items_.length on domain 42 equal to 2") << target;

    // Not "Must be present". The match already carries the direction, Add Rule
    // never asks for a presence here, and printing the default beside
    // "equal to 2" would put a second, contradictable answer in the row.
    EXPECT_EQ(table->item(0, 2)->text().toStdString(), "equal to 2");
}

TEST(FleetWindowRules, NamesARuleByItsDescriptionRatherThanItsGeneratedId) {
    Harness harness;
    Rule rule;
    // make_rule_id() derives ids like this one; nobody types them, and a column
    // of them tells an operator nothing about what is being checked.
    rule.id = "process-antivirus-exe";
    rule.description = "Antivirus must be running";
    rule.payload = ProcessRule{"antivirus.exe"};
    harness.controller->draft().templates.front().rules.push_back(rule);

    select_template(harness, QStringLiteral("Lab Workstation"));
    QTableWidget* table = rule_table(harness);
    ASSERT_NE(table, nullptr) << "no rule table";
    ASSERT_EQ(table->rowCount(), 1);

    EXPECT_EQ(table->item(0, 0)->text().toStdString(), "Antivirus must be running");
    // Not gone, just not the row label: an operator naming an exact rule in a
    // support conversation still needs it.
    EXPECT_NE(table->item(0, 0)->toolTip().indexOf(QStringLiteral("process-antivirus-exe")), -1)
        << table->item(0, 0)->toolTip().toStdString();
}

TEST(FleetWindowRules, FallsBackToTheTargetWhenTheDescriptionIsBlank) {
    Harness harness;
    Rule rule;
    rule.id = "process-antivirus-exe";
    rule.description = "";
    rule.payload = ProcessRule{"antivirus.exe"};
    harness.controller->draft().templates.front().rules.push_back(rule);

    select_template(harness, QStringLiteral("Lab Workstation"));
    QTableWidget* table = rule_table(harness);
    ASSERT_NE(table, nullptr);
    ASSERT_EQ(table->rowCount(), 1);

    // An empty first cell would read as a broken row rather than an
    // undescribed one, so describe() substitutes the kind and target.
    const QString label = table->item(0, 0)->text();
    EXPECT_FALSE(label.trimmed().isEmpty());
    EXPECT_NE(label.indexOf(QStringLiteral("antivirus.exe")), -1) << label.toStdString();
    EXPECT_EQ(label.indexOf(QStringLiteral("process-antivirus-exe")), -1) << label.toStdString();
}

TEST(FleetWindowRules, OffersBothDdsKindsInTheAddRuleDialog) {
    // The kind list now lives on the dialog that owns it; this only checks the
    // two DDS kinds reached it. Everything else about the dialog is covered in
    // test_add_rule_dialog.cpp.
    const QStringList kinds = AddRuleDialog::kind_choices();
    EXPECT_TRUE(kinds.contains(QStringLiteral("DDS: topic is published"))) << kinds.join(", ").toStdString();
    EXPECT_TRUE(kinds.contains(QStringLiteral("DDS: value on a topic"))) << kinds.join(", ").toStdString();
}
