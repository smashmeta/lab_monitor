#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lm/platform/fakes.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "monitor_worker.hpp"

using namespace lm::core;
using namespace lm::transport;
using namespace std::chrono_literals;

namespace {

/// A MonitorWorker on its own thread, over the in-memory bus, exactly as
/// main() wires one -- so these tests exercise the real threading rather than
/// a convenient approximation of it.
class ScriptTestHarness {
public:
    ScriptTestHarness(MessageBus& bus, std::string host_id, bool allow_scripts,
                      std::unique_ptr<lm::platform::IScriptRunner> runner) {
        lm::platform::ProbeSet probes;
        probes.resources = std::make_unique<lm::platform::FakeResourceProbe>();

        auto host_probes = std::make_unique<lm::platform::HostProbes>(
            std::move(host_id), std::move(probes), lm::core::platform_capabilities());

        worker_ = new MonitorWorker(std::move(host_probes), make_in_memory_client(bus),
                                    std::move(runner), allow_scripts);
        thread_ = new QThread();
        worker_->moveToThread(thread_);
        thread_->start();
        // Blocking, where main() posts this queued: start() is what subscribes
        // to the command topic, and that topic is Volatile -- a command
        // published before the subscription exists is dropped, not retained.
        // Returning only once start() has run is what stops a test racing it
        // and then passing because nothing ever ran.
        QMetaObject::invokeMethod(worker_, "start", Qt::BlockingQueuedConnection);
    }

    ~ScriptTestHarness() {
        thread_->quit();
        thread_->wait();
        // Deleted synchronously: the thread is joined, so nothing can still be
        // touching it, and deleteLater() would post to a queue nobody drains.
        delete worker_;
        delete thread_;
    }

    ScriptTestHarness(const ScriptTestHarness&) = delete;
    ScriptTestHarness& operator=(const ScriptTestHarness&) = delete;

    void force_announce() {
        QMetaObject::invokeMethod(worker_, "announce", Qt::QueuedConnection);
    }

private:
    MonitorWorker* worker_ = nullptr;
    QThread* thread_ = nullptr;
};

ScriptCommand command_for(const std::string& host, const std::string& run_id) {
    ScriptCommand command;
    command.host_id = host;
    command.run_id = run_id;
    command.script_name = "(custom script)";
    command.script_body = "exit 0";
    command.timeout_seconds = 5;
    return command;
}

/// Pumps the event loop until `done` or the deadline, so a queued result has a
/// chance to arrive without the test sleeping blindly.
void pump_until(const std::function<bool()>& done, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(10ms);
    }
}

}  // namespace

TEST(ClientScripts, RunsACommandAddressedToThisHostAndReportsTheOutcome) {
    MessageBus bus;
    std::vector<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    runner->next.exit_code = 0;
    runner->next.stdout_text = "did the thing";
    auto* runner_ptr = runner.get();

    // Harness: a worker with scripts enabled, on a host called PC-001.
    ScriptTestHarness harness(bus, "PC-001", /*allow_scripts=*/true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().status, ScriptStatus::Completed);
    EXPECT_EQ(results.front().run_id, "run-1");
    EXPECT_EQ(results.front().stdout_text, "did the thing");
    ASSERT_EQ(runner_ptr->bodies.size(), 1u);
    EXPECT_EQ(runner_ptr->bodies.front(), "exit 0");
}

TEST(ClientScripts, IgnoresACommandAddressedToAnotherHost) {
    MessageBus bus;
    std::vector<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-002", "run-1"));
    pump_until([&] { return !results.empty(); }, 500ms);

    EXPECT_TRUE(results.empty()) << "a host must not answer for another";
    EXPECT_TRUE(runner_ptr->bodies.empty());
}

TEST(ClientScripts, RefusesWhenNotEnrolled) {
    // The opt-in is what stops an agent upgrade silently turning a monitoring
    // box into one that runs remote code, so it must refuse *visibly* rather
    // than staying quiet -- an operator needs to know why nothing happened.
    MessageBus bus;
    std::vector<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", /*allow_scripts=*/false, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.front().status, ScriptStatus::Refused);
    EXPECT_FALSE(results.front().refusal_reason.empty());
    EXPECT_TRUE(runner_ptr->bodies.empty()) << "nothing may run when not enrolled";
}

TEST(ClientScripts, RunsARepeatedRunIdOnlyOnce) {
    // Volatile durability stops replay after a restart; it does not stop
    // redelivery within a session. Running an uninstall twice is the failure
    // this prevents.
    MessageBus bus;
    std::vector<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push_back(r); });

    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });
    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return results.size() > 1; }, 500ms);

    EXPECT_EQ(runner_ptr->bodies.size(), 1u) << "the same run must execute once";
}

TEST(ClientScripts, KeepsAnnouncingWhileAScriptIsRunning) {
    // The worker thread carries the 10 s announce. If execution sat on it, a
    // long script would push this host past its liveliness lease and the fleet
    // would watch it go Offline mid-run and then come back.
    MessageBus bus;
    std::atomic<bool> release{false};
    auto runner = std::make_unique<lm::platform::FakeScriptRunner>();
    runner->before_returning = [&] {
        while (!release) {
            std::this_thread::sleep_for(10ms);
        }
    };
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));

    // While the script is stuck, the worker must still service its own timers.
    std::vector<ClientAnnounce> announces;
    bus.subscribe_announce([&](const ClientAnnounce& a) { announces.push_back(a); });
    harness.force_announce();
    pump_until([&] { return !announces.empty(); });
    release = true;

    EXPECT_FALSE(announces.empty()) << "the monitoring thread was blocked by the script";
}
