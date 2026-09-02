#include <gtest/gtest.h>

#include <QApplication>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
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

    Harness() {
        EXPECT_TRUE(dir.isValid());
        controller = std::make_unique<ServerController>(make_in_memory_server(bus), dir.path());
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
