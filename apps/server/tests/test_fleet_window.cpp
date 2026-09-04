#include <gtest/gtest.h>

#include <QApplication>
#include <QCompleter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <variant>

#include "add_rule_dialog.hpp"
#include "fleet_window.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "lm/ui/fleet_model.hpp"
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

    /// The Templates tab's template list. Found by name rather than by being
    /// the only QListWidget in the window, which it stopped being when the
    /// Scripts tab arrived with a host list of its own.
    [[nodiscard]] QListWidget* template_list() const {
        return window->findChild<QListWidget*>(QStringLiteral("TemplateList"));
    }

    /// By object name, not by label. Matching on text swept every button in
    /// the window, and the window now spans four tabs -- "Clear" against
    /// "Clear filter" is all the headroom that was left, and the failure mode
    /// is a silently wrong widget.
    [[nodiscard]] QPushButton* button(const QString& name) const {
        return window->findChild<QPushButton*>(name);
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

/// The Fleet tab's host table.
///
/// Not simply findChild<QTableView*>(): QTableWidget derives from QTableView,
/// so the Templates tab's rule and assignment tables answer to that too and
/// whichever is found first wins. The host table is the only *plain* QTableView
/// here -- the only one driven by a model rather than by items.
QTableView* fleet_table(const Harness& harness) {
    for (QTableView* view : harness.window->findChildren<QTableView*>()) {
        if (qobject_cast<QTableWidget*>(view) == nullptr) {
            return view;
        }
    }
    return nullptr;
}

/// The compliance cell for one host, read through the *proxy* the view is
/// actually showing rather than off the model, so what is asserted is what is
/// on screen -- including the row order the proxy imposes.
QModelIndex compliance_index(const Harness& harness, const QString& host) {
    QTableView* table = fleet_table(harness);
    if (table == nullptr) {
        return {};
    }
    QAbstractItemModel* model = table->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex first = model->index(row, lm::ui::FleetModel::HostColumn);
        if (first.data(lm::ui::FleetModel::HostIdRole).toString() == host) {
            return model->index(row, lm::ui::FleetModel::ComplianceColumn);
        }
    }
    return {};
}

QString compliance_text(const Harness& harness, const QString& host) {
    return compliance_index(harness, host).data(Qt::DisplayRole).toString();
}

QStringList compliance_tag_labels(const Harness& harness, const QString& host) {
    const auto tags = compliance_index(harness, host)
                          .data(lm::ui::FleetModel::ComplianceTagsRole)
                          .value<QVector<lm::ui::ComplianceTag>>();
    QStringList labels;
    for (const lm::ui::ComplianceTag& tag : tags) {
        labels << tag.label;
    }
    return labels;
}

/// Host ids top to bottom, as the proxy orders them.
QStringList hosts_in_view(const Harness& harness) {
    QStringList hosts;
    QAbstractItemModel* model = fleet_table(harness)->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        hosts << model->index(row, lm::ui::FleetModel::HostColumn)
                     .data(lm::ui::FleetModel::HostIdRole)
                     .toString();
    }
    return hosts;
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
/// controller's signal directly: the row is filled by the controller's receive
/// path, which is the thing under test here.
void publish_report(Harness& harness, const ComplianceReport& report) {
    const auto client = make_in_memory_client(harness.bus);
    ComplianceReportMessage message;
    message.report = report;
    client->publish_report(message);
    QApplication::processEvents();
}

/// Puts a described rule into the template PC-001 is assigned and publishes the
/// bundle, which is what makes the description reachable from a report.
void publish_rule(Harness& harness, const std::string& id, const std::string& description) {
    Rule rule;
    rule.id = id;
    rule.description = description;
    rule.payload = ProcessRule{"antivirus.exe"};
    harness.controller->draft().templates.front().rules.push_back(rule);
    harness.controller->publish();
    QApplication::processEvents();
}

}  // namespace

TEST(FleetWindowCompliance, HasNoComplianceTab) {
    // One view, not two. The fleet table carries this now, and two views of the
    // same data that can disagree is worse than either alone.
    Harness harness;
    QTabWidget* tabs = harness.window->findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    for (int i = 0; i < tabs->count(); ++i) {
        EXPECT_NE(tabs->tabText(i), QStringLiteral("Compliance"));
    }
}

TEST(FleetWindowCompliance, ScoresEachHostAsPassedOverCheckedInTheFleetTable) {
    Harness harness;
    harness.controller->start();
    publish_report(harness, report_for("PC-001", {CheckStatus::Pass, CheckStatus::Pass,
                                                  CheckStatus::Fail, CheckStatus::NotApplicable}));

    // 2 of 3: the not-applicable rule is out of the denominator, because a rule
    // the client cannot evaluate can never pass.
    EXPECT_EQ(compliance_text(harness, QStringLiteral("PC-001")).toStdString(), "2 / 3");
}

TEST(FleetWindowCompliance, TagsAFailingRuleWithItsDescription) {
    Harness harness;
    harness.controller->start();
    publish_rule(harness, "process-antivirus-exe", "Antivirus must be running");

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"process-antivirus-exe", CheckStatus::Fail, "not running", ""}};
    publish_report(harness, report);

    EXPECT_EQ(compliance_tag_labels(harness, QStringLiteral("PC-001")).join(QChar(u'|')).toStdString(),
              "Antivirus must be running");
}

TEST(FleetWindowCompliance, PutsFailuresBeforeErrors) {
    // The cell truncates with "+N more", so this order decides what survives. A
    // rule that is definitely broken must not be pushed off the row by one that
    // merely could not be read.
    Harness harness;
    harness.controller->start();
    // Expected, so reporting makes it Online. An Unexpected host lists no tags
    // at all, which would make this pass without proving anything.
    harness.controller->add_expected_host("PC-001", "");

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"a", CheckStatus::Error, "denied", ""},
                      CheckResult{"b", CheckStatus::Fail, "not running", ""}};
    publish_report(harness, report);

    EXPECT_EQ(compliance_tag_labels(harness, QStringLiteral("PC-001")).join(QChar(u'|')).toStdString(),
              "b|a");
}

TEST(FleetWindowCompliance, ListsNeitherPassesNorNotApplicableAsTags) {
    // Passes are counted, never listed -- a wall of green pushes the one red
    // line that matters off the row. A rule the host cannot evaluate is not a
    // problem with the host.
    Harness harness;
    harness.controller->start();
    // Expected, so reporting makes it Online. An Unexpected host lists no tags
    // at all, which would make this pass without proving anything.
    harness.controller->add_expected_host("PC-001", "");
    publish_report(harness, report_for("PC-001", {CheckStatus::Pass, CheckStatus::NotApplicable}));

    EXPECT_TRUE(compliance_tag_labels(harness, QStringLiteral("PC-001")).isEmpty());
}

TEST(FleetWindowCompliance, FallsBackToTheRuleIdWhenTheRuleIsGone) {
    // The bundle changed between the client evaluating and this arriving.
    // Better a join key on screen than a blank tag with nothing to look up.
    Harness harness;
    harness.controller->start();
    // Expected, so reporting makes it Online. An Unexpected host lists no tags
    // at all, which would make this pass without proving anything.
    harness.controller->add_expected_host("PC-001", "");

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"vanished-rule", CheckStatus::Fail, "", ""}};
    publish_report(harness, report);

    EXPECT_EQ(compliance_tag_labels(harness, QStringLiteral("PC-001")).join(QChar(u'|')).toStdString(),
              "vanished-rule");
}

TEST(FleetWindowCompliance, SaysNotReportingForAHostThatNeverHas) {
    // Never "0 / 0", which on a machine nobody has heard from reads as a clean
    // bill of health rather than as the absence of one.
    Harness harness;
    harness.controller->start();
    harness.controller->add_expected_host("PC-dead", "");
    QApplication::processEvents();

    EXPECT_EQ(compliance_text(harness, QStringLiteral("PC-dead")).toStdString(), "Not reporting");
}

TEST(FleetWindowCompliance, NeverScoresAnUnreportedHostAsZeroOverZero) {
    Harness harness;
    harness.controller->start();
    harness.controller->add_expected_host("PC-002", "");
    QApplication::processEvents();

    EXPECT_NE(compliance_text(harness, QStringLiteral("PC-002")).toStdString(), "0 / 0");
}

TEST(FleetWindowCompliance, ListsNoTagsForAnUnexpectedHost) {
    // Its problem is that it is on the network at all. The failures are live
    // and true, and naming them on the row buries the one fact worth acting on
    // under detail about a machine nobody has agreed to manage.
    Harness harness;
    harness.controller->start();

    ComplianceReport report;
    report.host_id = "ROGUE";
    report.results = {CheckResult{"a", CheckStatus::Fail, "", ""}};
    publish_report(harness, report);

    ASSERT_EQ(compliance_text(harness, QStringLiteral("ROGUE")).toStdString(), "0 / 1")
        << "the ratio is a real reading and stays";
    EXPECT_TRUE(compliance_tag_labels(harness, QStringLiteral("ROGUE")).isEmpty());
}

TEST(FleetWindowCompliance, PutsTheWorstHostFirstAndSilentOnesAboveThem) {
    Harness harness;
    harness.controller->start();
    // Expected, so that reporting makes them Online. Without this they are
    // Unexpected, whose severity carries no ratio tie-break at all -- and the
    // order that came out was simply the order the reports arrived in, which
    // this assertion would have mistaken for worst-first.
    for (const char* host : {"PC-bad", "PC-mild", "PC-clean", "PC-dead"}) {
        harness.controller->add_expected_host(host, "");
    }
    publish_report(harness, report_for("PC-bad", {CheckStatus::Fail, CheckStatus::Fail}));
    publish_report(harness, report_for("PC-mild", {CheckStatus::Pass, CheckStatus::Fail}));
    publish_report(harness, report_for("PC-clean", {CheckStatus::Pass, CheckStatus::Pass}));
    QApplication::processEvents();

    const QStringList hosts = hosts_in_view(harness);
    const auto position = [&hosts](const char* host) { return hosts.indexOf(QLatin1String(host)); };
    const std::string order = hosts.join(QChar(u'|')).toStdString();
    EXPECT_LT(position("PC-dead"), position("PC-bad")) << order;
    EXPECT_LT(position("PC-bad"), position("PC-mild")) << order;
    EXPECT_LT(position("PC-mild"), position("PC-clean")) << order;
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

    QPushButton* remove = harness.button(QStringLiteral("RemoveTemplateButton"));
    ASSERT_NE(remove, nullptr);
    EXPECT_FALSE(remove->isEnabled()) << "greyed out beats silently doing nothing";
}

TEST(FleetWindowBaseline, EnablesRemovingAnOrdinaryTemplate) {
    Harness harness;
    QListWidget* list = harness.template_list();
    list->setCurrentRow(1);  // "Lab Workstation"
    QApplication::processEvents();

    EXPECT_TRUE(harness.button(QStringLiteral("RemoveTemplateButton"))->isEnabled());
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
    QPushButton* remove = harness.button(QStringLiteral("RemoveTemplateButton"));
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

namespace {

/// Announces the way a client does, so the whole path is exercised: the codec,
/// the registry, reconcile() and the model. Emitting the controller's signal
/// directly would skip the two places this feature actually lives.
void publish_announce(Harness& harness, const std::string& host, bool paused) {
    const auto client = make_in_memory_client(harness.bus);
    ClientAnnounce announce;
    announce.host_id = host;
    announce.agent_version = "test";
    announce.capabilities = 0;
    announce.paused = paused;
    client->publish_announce(announce);
    QApplication::processEvents();
}

QString state_text(const Harness& harness, const QString& host) {
    QTableView* table = fleet_table(harness);
    QAbstractItemModel* model = table->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, lm::ui::FleetModel::HostColumn)
                .data(lm::ui::FleetModel::HostIdRole)
                .toString() == host) {
            return model->index(row, lm::ui::FleetModel::StateColumn).data().toString();
        }
    }
    return {};
}

}  // namespace

TEST(FleetWindowPaused, APausedClientReadsAsPausedNotOnline) {
    Harness harness;
    harness.controller->start();
    harness.controller->add_expected_host("PC-001", "");
    publish_announce(harness, "PC-001", true);

    EXPECT_EQ(state_text(harness, QStringLiteral("PC-001")).toStdString(), "Paused");
    EXPECT_EQ(compliance_text(harness, QStringLiteral("PC-001")).toStdString(), "Paused");
}

TEST(FleetWindowPaused, ResumingPutsItBackOnline) {
    Harness harness;
    harness.controller->start();
    harness.controller->add_expected_host("PC-001", "");
    publish_announce(harness, "PC-001", true);
    ASSERT_EQ(state_text(harness, QStringLiteral("PC-001")).toStdString(), "Paused");

    publish_announce(harness, "PC-001", false);
    EXPECT_EQ(state_text(harness, QStringLiteral("PC-001")).toStdString(), "Online");
}

TEST(FleetWindowPaused, ListsNoTagsWhilePausedButKeepsTheLastKnownScore) {
    Harness harness;
    harness.controller->start();
    harness.controller->add_expected_host("PC-001", "");
    publish_announce(harness, "PC-001", false);

    ComplianceReport report;
    report.host_id = "PC-001";
    report.results = {CheckResult{"a", CheckStatus::Pass, "", ""},
                      CheckResult{"b", CheckStatus::Fail, "", ""}};
    publish_report(harness, report);
    ASSERT_FALSE(compliance_tag_labels(harness, QStringLiteral("PC-001")).isEmpty());

    publish_announce(harness, "PC-001", true);
    EXPECT_EQ(compliance_text(harness, QStringLiteral("PC-001")).toStdString(), "last known 1 / 2");
    EXPECT_TRUE(compliance_tag_labels(harness, QStringLiteral("PC-001")).isEmpty());
}


TEST(FleetWindowRules, KeepsARulesIdWhenItIsEdited) {
    // The id is the join key between this rule and the CheckResults already
    // reported for it. Regenerating on save would strip the labels off every
    // cached result until each client re-evaluated, up to 30 s later.
    Harness harness;
    Rule rule;
    rule.id = "process-antivirus-exe";
    rule.description = "Antivirus must be running";
    rule.payload = ProcessRule{"antivirus.exe"};
    harness.controller->draft().templates.front().rules.push_back(rule);

    select_template(harness, QStringLiteral("Lab Workstation"));
    QTableWidget* table = rule_table(harness);
    ASSERT_NE(table, nullptr);
    table->selectRow(0);

    // The dialog is modal, so the answer is scheduled before the click that
    // opens it -- the same trick the client's Close Program confirmation uses.
    QTimer::singleShot(0, [&harness] {
        auto* dialog = harness.window->findChild<AddRuleDialog*>(QStringLiteral("AddRuleDialog"));
        ASSERT_NE(dialog, nullptr);
        dialog->findChild<QLineEdit*>(QStringLiteral("ProcessExecutable"))
            ->setText(QStringLiteral("firefox.exe"));
        dialog->accept();
    });
    QPushButton* edit = harness.window->findChild<QPushButton*>(QStringLiteral("EditRuleButton"));
    ASSERT_NE(edit, nullptr) << "no Edit Rule button";
    edit->click();
    QApplication::processEvents();

    const auto& rules = harness.controller->draft().templates.front().rules;
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules.front().id, "process-antivirus-exe") << "the id must survive the edit";
    EXPECT_EQ(std::get<ProcessRule>(rules.front().payload).executable, "firefox.exe")
        << "the edit must actually have been applied";
}

TEST(FleetWindowRules, EditingIsReachableByDoubleClickingARow) {
    // The button makes it discoverable; the double-click is what anyone tries
    // first on a table. Both have to reach the same place.
    Harness harness;
    Rule rule;
    rule.id = "process-antivirus-exe";
    rule.payload = ProcessRule{"antivirus.exe"};
    harness.controller->draft().templates.front().rules.push_back(rule);

    select_template(harness, QStringLiteral("Lab Workstation"));
    QTableWidget* table = rule_table(harness);
    ASSERT_NE(table, nullptr);
    table->selectRow(0);

    bool opened = false;
    QTimer::singleShot(0, [&harness, &opened] {
        auto* dialog = harness.window->findChild<AddRuleDialog*>(QStringLiteral("AddRuleDialog"));
        if (dialog != nullptr) {
            opened = dialog->is_editing();
            dialog->reject();
        }
    });
    emit table->itemDoubleClicked(table->item(0, 0));
    QApplication::processEvents();

    EXPECT_TRUE(opened) << "a double-click did not open the rule for editing";
}
