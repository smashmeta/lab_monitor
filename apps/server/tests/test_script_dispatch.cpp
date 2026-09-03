#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

#include "lm/transport/in_memory_transport.hpp"
#include "server_controller.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

/// A controller over an in-memory bus, with the bus reachable so a test can
/// play the part of a client: announcing hosts and publishing results.
struct Harness {
    MessageBus bus;
    QTemporaryDir dir;
    std::unique_ptr<ServerController> controller;
    std::vector<ScriptCommand> dispatched;

    /// The deadline margin is a constructor seam so the deadline tests below
    /// can run in milliseconds rather than the fifteen seconds the product
    /// waits. Everything else uses the real default.
    explicit Harness(
        std::chrono::milliseconds margin = ServerController::kDefaultDeadlineMargin) {
        EXPECT_TRUE(dir.isValid());
        controller =
            std::make_unique<ServerController>(make_in_memory_server(bus), dir.path(), nullptr,
                                               margin);
        bus.subscribe_script_command(
            [this](const ScriptCommand& command) { dispatched.push_back(command); });
        controller->start();
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

    void publish_result(const std::string& host, const std::string& run_id, ScriptStatus status) {
        const auto client = make_in_memory_client(bus);
        ScriptResultMessage message;
        message.host_id = host;
        message.run_id = run_id;
        message.status = status;
        client->publish_script_result(message);
        QApplication::processEvents();
    }
};

Capabilities enrolled() {
    Capabilities caps;
    caps.add(Capability::Resources).add(Capability::Scripts);
    return caps;
}

const RunTarget& target_for(const ScriptRun& run, const std::string& host) {
    const auto found = std::ranges::find(run.targets, host, &RunTarget::host_id);
    EXPECT_NE(found, run.targets.end());
    return *found;
}

}  // namespace

TEST(ScriptDispatch, SendsOneCommandPerTargetedHost) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());

    const QString run_id = harness.controller->start_script_run(
        "(custom script)", "exit 0", {"PC-001", "PC-002"}, 60);

    ASSERT_EQ(harness.dispatched.size(), 2u);
    EXPECT_NE(harness.dispatched[0].host_id, harness.dispatched[1].host_id)
        << "each host gets its own sample, keyed by host";
    EXPECT_EQ(harness.dispatched[0].run_id, run_id.toStdString());
    EXPECT_EQ(harness.dispatched[1].run_id, run_id.toStdString());
    EXPECT_EQ(harness.dispatched[0].script_body, "exit 0");
}

TEST(ScriptDispatch, RefusesAHostThatIsNotOnlineWithoutSendingAnything) {
    // The topic is Volatile, so there is no queue to hold a command until the
    // machine returns. Promising delivery would be a lie, so it is refused up
    // front and the operator is told immediately rather than after a deadline.
    Harness harness;
    harness.controller->add_expected_host("PC-gone", "");
    QApplication::processEvents();

    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-gone"}, 60);

    EXPECT_TRUE(harness.dispatched.empty()) << "nothing may be sent to an absent host";
    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(run.run_id, run_id.toStdString());
    EXPECT_EQ(target_for(run, "PC-gone").state, TargetState::Refused);
    EXPECT_FALSE(target_for(run, "PC-gone").detail.empty());
    // The reason names the state rather than summarising it as "not online": a
    // Paused host is online and merely silent, and Offline, Missing and
    // Unexpected each send the reader somewhere different.
    EXPECT_NE(target_for(run, "PC-gone").detail.find(to_string(HostState::Missing)),
              std::string::npos)
        << target_for(run, "PC-gone").detail;
}

TEST(ScriptDispatch, RefusesAHostWithoutTheScriptsCapability) {
    // The opt-in is the whole bound on this feature, so the server honours it
    // too rather than sending and letting the client decline -- an un-enrolled
    // machine should never receive a script body at all.
    Harness harness;
    Capabilities bare;
    bare.add(Capability::Resources);
    harness.announce("PC-001", bare);

    harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 60);

    EXPECT_TRUE(harness.dispatched.empty());
    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::Refused);
    EXPECT_NE(target_for(run, "PC-001").detail.find("enrol"), std::string::npos)
        << target_for(run, "PC-001").detail;
}

TEST(ScriptDispatch, CorrelatesAResultBackToItsRunAndHost) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    const QString run_id = harness.controller->start_script_run(
        "(custom script)", "exit 0", {"PC-001", "PC-002"}, 60);

    harness.publish_result("PC-002", run_id.toStdString(), ScriptStatus::Completed);

    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-002").state, TargetState::Completed);
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::Dispatched)
        << "one host answering must not move the others";
}

TEST(ScriptDispatch, IgnoresAResultForARunItDoesNotKnow) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 60);

    harness.publish_result("PC-001", "some-other-run", ScriptStatus::Completed);

    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::Dispatched)
        << "a stray run_id must not move this run";
    EXPECT_EQ(run.run_id, run_id.toStdString());
}

TEST(ScriptDispatch, EmitsRunChangedWhenATargetMoves) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 60);

    QSignalSpy spy(harness.controller.get(), &ServerController::script_run_changed);
    ASSERT_TRUE(spy.isValid());

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    ASSERT_GT(spy.count(), 0) << "the view has no other way to know a result arrived";
    EXPECT_EQ(spy.front().at(0).toString(), run_id);
}

TEST(ScriptDispatch, ARefusedHostIsNotMovedByAResultCarryingTheRunId) {
    // The enrolment opt-in is the whole bound on this feature. A result from a
    // host that was never sent the script cannot be an answer to it, and
    // letting one through would leave the audit record saying an un-enrolled
    // machine ran it.
    Harness harness;
    Capabilities bare;
    bare.add(Capability::Resources);
    harness.announce("PC-001", bare);
    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 60);

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::Refused);
    EXPECT_NE(target_for(run, "PC-001").detail.find("enrol"), std::string::npos)
        << "the refusal reason must survive: it is what the audit trail says happened";
}

TEST(ScriptDispatch, TurnsASilentHostIntoNoResponseWhenTheDeadlinePasses) {
    // The only wiring for one of the four terminal outcomes. Driven through a
    // millisecond margin rather than a sleep, so the suite stays fast.
    Harness harness{std::chrono::milliseconds{5}};
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 0);
    ASSERT_EQ(target_for(harness.controller->script_runs().back(), "PC-001").state,
              TargetState::Dispatched);

    QSignalSpy spy(harness.controller.get(), &ServerController::script_run_changed);
    ASSERT_TRUE(spy.isValid());
    ASSERT_TRUE(spy.wait(5000)) << "the deadline timer never fired";

    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::NoResponse);
    EXPECT_TRUE(run.is_finished());
    EXPECT_EQ(spy.front().at(0).toString(), run_id);
}

TEST(ScriptDispatch, DoesNotArmADeadlineForARunThatDispatchedToNobody) {
    // Every target is already terminal, so a deadline has nothing it could
    // move -- and a run_changed for a run nothing happened to would have the
    // view redraw a finished run on a timer.
    Harness harness{std::chrono::milliseconds{5}};
    harness.controller->add_expected_host("PC-gone", "");
    QApplication::processEvents();
    harness.controller->start_script_run("(custom script)", "exit 0", {"PC-gone"}, 0);

    QSignalSpy spy(harness.controller.get(), &ServerController::script_run_changed);
    ASSERT_TRUE(spy.isValid());
    QTest::qWait(80);

    EXPECT_EQ(spy.count(), 0) << "no deadline should have been armed";
    EXPECT_EQ(target_for(harness.controller->script_runs().back(), "PC-gone").state,
              TargetState::Refused);
}

TEST(ScriptDispatch, TheDeadlineSaysNothingAboutARunEverybodyAnswered) {
    // A wide margin here on purpose, unlike the tests above. The result has to
    // be *processed* before the deadline fires, and a 5 ms margin loses that
    // race under load -- the deadline event and the queued result are both
    // sitting in the same queue, and this test then sees two emissions for a
    // reason that has nothing to do with what it is checking. Observed, not
    // theorised: it passed alone and failed in the full run.
    Harness harness{std::chrono::milliseconds{300}};
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("(custom script)", "exit 0", {"PC-001"}, 0);

    QSignalSpy spy(harness.controller.get(), &ServerController::script_run_changed);
    ASSERT_TRUE(spy.isValid());
    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    QTest::qWait(600);  // long past the 300 ms margin

    const ScriptRun& run = harness.controller->script_runs().back();
    EXPECT_EQ(target_for(run, "PC-001").state, TargetState::Completed)
        << "a deadline must never overwrite an answer";
    EXPECT_EQ(spy.count(), 1) << "only the result moved anything; the deadline had nothing to say";
}

TEST(ScriptShareRoot, PersistsAcrossARestart) {
    // A share moves, or an operator is handed a new one, far more often than
    // this console is restarted -- so it is a setting beside the bundle, not a
    // launch option (spec section 8).
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString share = QStringLiteral("C:/scripts/share");

    {
        MessageBus bus;
        ServerController controller(make_in_memory_server(bus), dir.path());
        controller.start();
        controller.set_script_share_root(share);
        controller.stop();
    }

    MessageBus bus;
    ServerController reopened(make_in_memory_server(bus), dir.path());
    reopened.start();
    EXPECT_EQ(reopened.script_share_root(), share);
    reopened.stop();
}

TEST(ScriptShareRoot, AnnouncesAChangeSoTheTabCanReread) {
    Harness harness;
    QSignalSpy spy(harness.controller.get(), &ServerController::script_share_root_changed);
    ASSERT_TRUE(spy.isValid());

    harness.controller->set_script_share_root(QStringLiteral("C:/scripts"));

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.front().at(0).toString(), QStringLiteral("C:/scripts"));

    harness.controller->set_script_share_root(QStringLiteral("C:/scripts"));
    EXPECT_EQ(spy.count(), 1) << "setting the same path again is not a change";
}

TEST(ScriptShareRoot, StartsEmptyOnAFreshConfigDirectory) {
    Harness harness;
    EXPECT_TRUE(harness.controller->script_share_root().isEmpty())
        << "an unconfigured share must not read as a configured one";
}

TEST(ScriptShareRoot, AnnouncesTheLoadedRootWhenStartIsCalled) {
    // main.cpp constructs FleetWindow (and so ScriptsTab, and its connection to
    // script_share_root_changed) *before* calling controller->start() -- so a
    // root loaded from disk has to announce itself from inside start(), not
    // just sit in the getter for a tab that already read an empty string and
    // was never told to look again. The spy is connected before start() runs,
    // which is the whole point: connecting after would pass while this bug
    // was live.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString share = QStringLiteral("C:/scripts/share");

    {
        MessageBus bus;
        ServerController controller(make_in_memory_server(bus), dir.path());
        controller.start();
        controller.set_script_share_root(share);
        controller.stop();
    }

    MessageBus bus;
    ServerController reopened(make_in_memory_server(bus), dir.path());
    QSignalSpy spy(&reopened, &ServerController::script_share_root_changed);
    ASSERT_TRUE(spy.isValid());

    reopened.start();

    ASSERT_EQ(spy.count(), 1)
        << "the persisted root must be announced on load, not just returned by the getter";
    EXPECT_EQ(spy.front().at(0).toString(), share);
    reopened.stop();
}

TEST(ScriptShareRoot, EmitsNothingOnStartWithAFreshConfigDirectory) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    MessageBus bus;
    ServerController controller(make_in_memory_server(bus), dir.path());
    QSignalSpy spy(&controller, &ServerController::script_share_root_changed);
    ASSERT_TRUE(spy.isValid());

    controller.start();

    EXPECT_EQ(spy.count(), 0)
        << "an unconfigured share is the starting state, not a change to announce";
    controller.stop();
}
