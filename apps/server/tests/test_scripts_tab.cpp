#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fleet_window.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "script_run.hpp"
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
};

/// What a machine enrolled for remote execution advertises.
Capabilities enrolled() {
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

/// The host id a row stands for, with any "— why not" suffix stripped.
QString host_id_of(const QListWidgetItem* item) {
    return item->text().section(QStringLiteral(" — "), 0, 0);
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
