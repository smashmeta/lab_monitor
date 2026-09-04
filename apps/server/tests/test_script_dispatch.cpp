#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QIODevice>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <chrono>
#include <memory>
#include <vector>

#include "lm/transport/in_memory_transport.hpp"
#include "run_store.hpp"
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

TEST(ScriptShareRoot, ReportsAMalformedSettingsFileAndLeavesTheRootEmpty) {
    // Valid JSON, wrong shape -- not the {"share_root": "..."} object load_config()
    // expects. This must take the same "could not be read" branch as unparseable
    // JSON, not silently adopt something that happens to parse.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        QFile file(dir.path() + QStringLiteral("/scripts.json"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("[]");
    }

    MessageBus bus;
    ServerController controller(make_in_memory_server(bus), dir.path());
    QSignalSpy spy(&controller, &ServerController::config_error);
    ASSERT_TRUE(spy.isValid());

    controller.start();

    EXPECT_TRUE(controller.script_share_root().isEmpty())
        << "a malformed settings file must not read as a configured share";
    EXPECT_EQ(spy.count(), 1);
    controller.stop();
}

TEST(ScriptShareRoot, ReportsAnUnparseableSettingsFileTheSameWay) {
    // Pins that nlohmann::json::parse's allow_exceptions=false is actually
    // doing its job here, rather than something throwing past it.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        QFile file(dir.path() + QStringLiteral("/scripts.json"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{ this is not json");
    }

    MessageBus bus;
    ServerController controller(make_in_memory_server(bus), dir.path());
    QSignalSpy spy(&controller, &ServerController::config_error);
    ASSERT_TRUE(spy.isValid());

    controller.start();

    EXPECT_TRUE(controller.script_share_root().isEmpty())
        << "unparseable JSON must not read as a configured share";
    EXPECT_EQ(spy.count(), 1);
    controller.stop();
}

TEST(RunHistory, KeepsAFinishedRunAcrossARestart) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string run_id;

    {
        MessageBus bus;
        ServerController controller(make_in_memory_server(bus), dir.path());
        controller.start();
        const auto client = make_in_memory_client(bus);
        ClientAnnounce announce;
        announce.host_id = "PC-001";
        announce.capabilities = enrolled().raw();
        client->publish_announce(announce);
        controller.add_expected_host("PC-001", "");
        QApplication::processEvents();

        run_id = controller.start_script_run("a.ps1", "exit 0", {"PC-001"}, 60).toStdString();
        ScriptResultMessage result;
        result.host_id = "PC-001";
        result.run_id = run_id;
        result.status = ScriptStatus::Completed;
        client->publish_script_result(result);
        QApplication::processEvents();
        controller.stop();
    }

    MessageBus bus;
    ServerController reopened(make_in_memory_server(bus), dir.path());
    reopened.start();

    ASSERT_EQ(reopened.script_runs().size(), 1u) << "the audit trail did not survive";
    EXPECT_EQ(reopened.script_runs().front().run_id, run_id);
    EXPECT_EQ(reopened.script_runs().front().targets.front().state, TargetState::Completed);
    reopened.stop();
}

TEST(RunHistory, AnnouncesLoadedRunsWhenStartIsCalled) {
    // main.cpp constructs FleetWindow (and so Task 9's history list, reading
    // script_runs() from its constructor) *before* calling controller->start()
    // -- the exact trap this branch was already bitten by once, on the share
    // root (ScriptShareRoot.AnnouncesTheLoadedRootWhenStartIsCalled, below). A
    // run loaded from disk has to announce itself from inside start(), not
    // just sit in script_runs() for a view that already read an empty vector
    // and was never told to look again. KeepsAFinishedRunAcrossARestart above
    // pins that the runs are *loaded*; this pins that anyone is *told* --
    // different failures with the same symptom on screen. The spy is
    // connected before start() runs, which is the whole point: connecting
    // after would pass while this bug was live.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string run_id;

    {
        MessageBus bus;
        ServerController controller(make_in_memory_server(bus), dir.path());
        controller.start();
        const auto client = make_in_memory_client(bus);
        ClientAnnounce announce;
        announce.host_id = "PC-001";
        announce.capabilities = enrolled().raw();
        client->publish_announce(announce);
        controller.add_expected_host("PC-001", "");
        QApplication::processEvents();

        run_id = controller.start_script_run("a.ps1", "exit 0", {"PC-001"}, 60).toStdString();
        ScriptResultMessage result;
        result.host_id = "PC-001";
        result.run_id = run_id;
        result.status = ScriptStatus::Completed;
        client->publish_script_result(result);
        QApplication::processEvents();
        controller.stop();
    }

    MessageBus bus;
    ServerController reopened(make_in_memory_server(bus), dir.path());
    QSignalSpy spy(&reopened, &ServerController::script_runs_changed);
    ASSERT_TRUE(spy.isValid());

    reopened.start();

    ASSERT_GT(spy.count(), 0)
        << "the persisted run must be announced on load, not just returned by the getter";
    ASSERT_EQ(reopened.script_runs().size(), 1u);
    EXPECT_EQ(reopened.script_runs().front().run_id, run_id);
    reopened.stop();
}

TEST(RunHistory, EmitsNothingOnStartWithAFreshConfigDirectory) {
    // A fresh config directory with no runs/ is the normal starting state,
    // not a change -- a spurious emit here would have the history list
    // rebuilding for nothing on every clean launch.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    MessageBus bus;
    ServerController controller(make_in_memory_server(bus), dir.path());
    QSignalSpy spy(&controller, &ServerController::script_runs_changed);
    ASSERT_TRUE(spy.isValid());

    controller.start();

    EXPECT_EQ(spy.count(), 0)
        << "an empty run history is the starting state, not a change to announce";
    EXPECT_TRUE(controller.script_runs().empty());
    controller.stop();
}

TEST(RunHistory, WritesNoFileUntilEveryTargetIsTerminal) {
    // The actual invariant, now that persist_run_if_finished() can rewrite an
    // already-saved run: not "saved exactly once", but "nothing is written
    // until the run finishes" -- which is what the hundred-writes concern was
    // about in the first place. Every other test here finishes a run with a
    // single host and a single event, so deleting the is_finished() guard
    // entirely would fail nothing else in this file.
    Harness harness;
    harness.announce("PC-001", enrolled());
    harness.announce("PC-002", enrolled());
    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-001", "PC-002"}, 60);
    const QString run_file =
        harness.controller->runs_dir() + QStringLiteral("/") + run_id + QStringLiteral(".json");

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    EXPECT_FALSE(QFile::exists(run_file))
        << "one of two targets answering must not write anything yet";

    harness.publish_result("PC-002", run_id.toStdString(), ScriptStatus::Completed);
    EXPECT_TRUE(QFile::exists(run_file))
        << "the last target finishing is what makes the run an audit record";
}

TEST(RunHistory, KeepsARunThatDispatchedToNobody) {
    // A run whose every target was Refused at dispatch -- no host Online, none
    // enrolled -- is is_finished() the moment it is created: no result will
    // ever arrive for it, and start_script_run() deliberately arms no deadline
    // when nothing went out. So the two callers of persist_run_if_finished()
    // that existed before this test (on_script_result, on_run_deadline) can
    // never run for it, and it lived in the History list for the session and
    // was gone at the next launch.
    //
    // It is precisely the run worth auditing: somebody pushed a script at a
    // machine and it went nowhere. Read back with load_runs() rather than
    // trusting script_runs(), which would pass on the in-memory entry alone
    // while nothing was ever written.
    Harness harness;
    harness.controller->add_expected_host("PC-gone", "");
    QApplication::processEvents();

    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-gone"}, 60);

    ASSERT_TRUE(harness.dispatched.empty()) << "premise: nothing was sent to anybody";
    ASSERT_EQ(harness.controller->script_runs().size(), 1u);
    ASSERT_EQ(target_for(harness.controller->script_runs().back(), "PC-gone").state,
              TargetState::Refused)
        << "premise: the run is finished from birth, with nothing left to move it";

    std::vector<QString> errors;
    const std::vector<ScriptRun> on_disk = load_runs(harness.controller->runs_dir(), &errors);
    ASSERT_EQ(on_disk.size(), 1u)
        << "a run that reached nobody is still an audit record, and must survive a restart";
    EXPECT_EQ(on_disk.front().run_id, run_id.toStdString());
    EXPECT_EQ(target_for(on_disk.front(), "PC-gone").state, TargetState::Refused);
}

TEST(RunHistory, DeletingARunThatWasNeverWrittenIsNotAnError) {
    // A run's History row exists from creation, so Run -> select it -> Delete
    // is an ordinary sequence on a run still in flight -- which has no file,
    // because nothing is written until a run finishes. delete_run() reporting
    // "No such run" for that reached config_error(), which FleetWindow turns
    // into a modal titled "Configuration error" over an action that did
    // exactly what was asked. A false error on a destructive action trains
    // people to dismiss the real ones.
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-001"}, 60);
    ASSERT_EQ(target_for(harness.controller->script_runs().back(), "PC-001").state,
              TargetState::Dispatched)
        << "premise: still live, so nothing has been written for this run";
    ASSERT_FALSE(QFile::exists(harness.controller->runs_dir() + QStringLiteral("/") + run_id +
                               QStringLiteral(".json")));

    QSignalSpy spy(harness.controller.get(), &ServerController::config_error);
    ASSERT_TRUE(spy.isValid());

    harness.controller->delete_script_run(run_id.toStdString());

    EXPECT_EQ(spy.count(), 0) << "a run that was never written is not an error to delete";
    EXPECT_TRUE(harness.controller->script_runs().empty())
        << "and it still has to go from the history";
}

TEST(RunHistory, CorrectsTheFileOnDiskWhenALateResultArrivesAfterTheDeadline) {
    // A deadline fires first, saving NoResponse for a host that turns out to
    // have merely been slow. on_script_result() still accepts a result for a
    // target already at NoResponse -- a late answer is exactly what an
    // operator wants to see -- and the file on disk, not just the in-memory
    // run, has to end up saying so: read back with load_runs() rather than
    // trusting script_runs(), which would pass even if the on-disk record
    // were never rewritten.
    Harness harness{std::chrono::milliseconds{5}};
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-001"}, 0);

    QSignalSpy spy(harness.controller.get(), &ServerController::script_run_changed);
    ASSERT_TRUE(spy.isValid());
    ASSERT_TRUE(spy.wait(5000)) << "the deadline timer never fired";
    ASSERT_EQ(target_for(harness.controller->script_runs().back(), "PC-001").state,
              TargetState::NoResponse);

    {
        std::vector<QString> errors;
        const std::vector<ScriptRun> on_disk = load_runs(harness.controller->runs_dir(), &errors);
        ASSERT_EQ(on_disk.size(), 1u) << "the deadline must have written the file already";
        EXPECT_EQ(target_for(on_disk.front(), "PC-001").state, TargetState::NoResponse);
    }

    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);
    EXPECT_EQ(target_for(harness.controller->script_runs().back(), "PC-001").state,
              TargetState::Completed)
        << "the in-memory run must accept the late answer";

    std::vector<QString> errors;
    const std::vector<ScriptRun> on_disk = load_runs(harness.controller->runs_dir(), &errors);
    ASSERT_EQ(on_disk.size(), 1u);
    EXPECT_EQ(target_for(on_disk.front(), "PC-001").state, TargetState::Completed)
        << "the record on disk must be corrected, not left saying NoResponse forever";
}

TEST(RunHistory, DeletesOneRunAndSaysItChanged) {
    Harness harness;
    harness.announce("PC-001", enrolled());
    const QString run_id =
        harness.controller->start_script_run("a.ps1", "exit 0", {"PC-001"}, 60);
    harness.publish_result("PC-001", run_id.toStdString(), ScriptStatus::Completed);

    QSignalSpy spy(harness.controller.get(), &ServerController::script_runs_changed);
    ASSERT_TRUE(spy.isValid());

    EXPECT_TRUE(harness.controller->delete_script_run(run_id.toStdString()));
    EXPECT_TRUE(harness.controller->script_runs().empty());
    EXPECT_GT(spy.count(), 0) << "the history view has no other way to know";
}

TEST(RunHistory, DeletesOnlyRunsOlderThanTheCutoff) {
    // No automatic pruning anywhere (spec section 8): this runs when an
    // operator asks, and takes exactly what they asked for -- which means
    // this test needs one run genuinely older than the cutoff and one
    // genuinely newer *in the same call*, or a version that deleted
    // everything regardless of the argument would pass unchanged. A run
    // issued 48 hours ago is written straight to disk with save_run(),
    // before the controller starts and loads it back -- no sleeping
    // required to get a run older than an hour.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString runs_dir = dir.path() + QStringLiteral("/runs");

    ScriptRun old_run;
    old_run.run_id = "old-run";
    old_run.script_name = "old.ps1";
    old_run.script_body = "exit 0";
    old_run.issued_at = std::chrono::system_clock::now() - std::chrono::hours(48);
    old_run.timeout_seconds = 60;
    old_run.targets.push_back(RunTarget{"PC-001", TargetState::Completed, {}, {}});
    QString save_error;
    ASSERT_TRUE(save_run(runs_dir, old_run, &save_error)) << save_error.toStdString();

    MessageBus bus;
    ServerController controller(make_in_memory_server(bus), dir.path());
    controller.start();
    ASSERT_EQ(controller.script_runs().size(), 1u) << "the old run must have loaded";

    const auto client = make_in_memory_client(bus);
    ClientAnnounce announce;
    announce.host_id = "PC-002";
    announce.capabilities = enrolled().raw();
    client->publish_announce(announce);
    controller.add_expected_host("PC-002", "");
    QApplication::processEvents();

    // Issued just now, in this session -- genuinely newer than the cutoff
    // below, unlike the loaded run.
    const QString recent = controller.start_script_run("new.ps1", "exit 0", {"PC-002"}, 60);
    ScriptResultMessage result;
    result.host_id = "PC-002";
    result.run_id = recent.toStdString();
    result.status = ScriptStatus::Completed;
    client->publish_script_result(result);
    QApplication::processEvents();
    ASSERT_EQ(controller.script_runs().size(), 2u);

    EXPECT_EQ(controller.delete_script_runs_before(std::chrono::system_clock::now() -
                                                    std::chrono::hours(1)),
              1u)
        << "only the 48-hour-old run is older than an hour ago; a version that ignored the "
           "cutoff and deleted everything would also report a wrong count here";
    ASSERT_EQ(controller.script_runs().size(), 1u);
    EXPECT_EQ(controller.script_runs().front().run_id, recent.toStdString())
        << "the recent run must survive; only the old one was asked for";

    EXPECT_EQ(controller.delete_script_runs_before(std::chrono::system_clock::now() -
                                                     std::chrono::hours(24)),
              0u)
        << "nothing left is older than yesterday, and nothing may be taken";
    controller.stop();
}
