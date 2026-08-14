#include <gtest/gtest.h>

#include <QApplication>
#include <QCompleter>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstddef>
#include <memory>

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

    QCompleter* completer = editor->findChild<QLineEdit*>()->completer();
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
