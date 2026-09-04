#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
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

/// Everything these tests collect is written on the worker thread (results) or
/// the script thread (bodies) and read from this one. QCoreApplication::
/// processEvents() drains only the main thread's own queue and establishes no
/// happens-before edge with either, so the lock is not belt-and-braces: it is
/// the only ordering there is.
template <typename T>
class Guarded {
public:
    void push(const T& value) {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.push_back(value);
    }

    [[nodiscard]] std::size_t size() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    /// By value: a reference would escape the lock it was read under.
    [[nodiscard]] T at(std::size_t index) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return items_.at(index);
    }

    void clear() {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<T> items_;
};

/// FakeScriptRunner records `bodies` on whichever thread called run(), and
/// lm_platform's own tests -- which are single-threaded -- are the reason it
/// does so unguarded. The synchronisation belongs here rather than in the
/// shared fake, so this records its own copy and the base's vector is never
/// read.
///
/// Recorded *before* delegating, so bodies() reports the run as started while
/// before_returning is still blocking inside it. That is what lets a test wait
/// for a script to be genuinely under way rather than assuming it.
class RecordingRunner : public lm::platform::FakeScriptRunner {
public:
    lm::core::ScriptOutcome run(const std::string& body,
                                std::chrono::seconds timeout) override {
        bodies_.push(body);
        return FakeScriptRunner::run(body, timeout);
    }

    [[nodiscard]] std::size_t body_count() const { return bodies_.size(); }
    [[nodiscard]] std::string body_at(std::size_t index) const { return bodies_.at(index); }

private:
    Guarded<std::string> bodies_;
};

/// IScriptRunner promises nothing about throwing, and the run thread is a bare
/// std::thread -- an escaping exception there is std::terminate.
class ThrowingRunner : public lm::platform::IScriptRunner {
public:
    lm::core::ScriptOutcome run(const std::string&, std::chrono::seconds) override {
        throw std::runtime_error("no shell today");
    }
};

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
    // Declared before the harness throughout: the bus holds a lambda capturing
    // this by reference, and the worker thread is still alive until the
    // harness destructor runs.
    Guarded<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push(r); });

    auto runner = std::make_unique<RecordingRunner>();
    runner->next.exit_code = 0;
    runner->next.stdout_text = "did the thing";
    auto* runner_ptr = runner.get();

    // Harness: a worker with scripts enabled, on a host called PC-001.
    ScriptTestHarness harness(bus, "PC-001", /*allow_scripts=*/true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.at(0).status, ScriptStatus::Completed);
    EXPECT_EQ(results.at(0).run_id, "run-1");
    EXPECT_EQ(results.at(0).stdout_text, "did the thing");
    ASSERT_EQ(runner_ptr->body_count(), 1u);
    EXPECT_EQ(runner_ptr->body_at(0), "exit 0");
}

TEST(ClientScripts, IgnoresACommandAddressedToAnotherHost) {
    MessageBus bus;
    Guarded<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push(r); });

    auto runner = std::make_unique<RecordingRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-002", "run-1"));
    pump_until([&] { return !results.empty(); }, 500ms);

    EXPECT_TRUE(results.empty()) << "a host must not answer for another";
    EXPECT_EQ(runner_ptr->body_count(), 0u);
}

TEST(ClientScripts, RefusesWhenNotEnrolled) {
    // The opt-in is what stops an agent upgrade silently turning a monitoring
    // box into one that runs remote code, so it must refuse *visibly* rather
    // than staying quiet -- an operator needs to know why nothing happened.
    MessageBus bus;
    Guarded<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push(r); });

    auto runner = std::make_unique<RecordingRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", /*allow_scripts=*/false, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.at(0).status, ScriptStatus::Refused);
    // Names the flag, because this is the refusal an operator can do something
    // about -- unlike the platform that has no runner at all.
    EXPECT_NE(results.at(0).refusal_reason.find("--allow-scripts"), std::string::npos);
    EXPECT_EQ(runner_ptr->body_count(), 0u) << "nothing may run when not enrolled";
}

TEST(ClientScripts, RefusesWithoutARunnerWithoutBlamingTheFlag) {
    // A platform with no runner (Linux, today) is not an enrolment problem, and
    // telling that operator to pass --allow-scripts sends them to try a flag
    // they have already passed.
    MessageBus bus;
    Guarded<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push(r); });

    ScriptTestHarness harness(bus, "PC-001", /*allow_scripts=*/true, nullptr);

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.at(0).status, ScriptStatus::Refused);
    EXPECT_FALSE(results.at(0).refusal_reason.empty());
    EXPECT_EQ(results.at(0).refusal_reason.find("--allow-scripts"), std::string::npos);
}

TEST(ClientScripts, RunsARepeatedRunIdOnlyOnce) {
    // Volatile durability stops replay after a restart; it does not stop
    // redelivery within a session. Running an uninstall twice is the failure
    // this prevents.
    MessageBus bus;
    Guarded<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push(r); });

    auto runner = std::make_unique<RecordingRunner>();
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });
    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return results.size() > 1; }, 500ms);

    EXPECT_EQ(runner_ptr->body_count(), 1u) << "the same run must execute once";
}

TEST(ClientScripts, LetsARunRefusedForBeingConcurrentBeRetried) {
    // The subtle half of the duplicate guard. A run_id is recorded before the
    // busy check, so refusing for "already running" has to un-record it --
    // otherwise the id is remembered as executed by a run that never happened,
    // and the operator's retry is silently swallowed as a duplicate forever.
    MessageBus bus;
    std::atomic<bool> release{false};
    Guarded<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push(r); });

    auto runner = std::make_unique<RecordingRunner>();
    runner->before_returning = [&] {
        while (!release) {
            std::this_thread::sleep_for(10ms);
        }
    };
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return runner_ptr->body_count() == 1u; });
    ASSERT_EQ(runner_ptr->body_count(), 1u) << "run-1 must hold the runner open";

    bus.publish_script_command(command_for("PC-001", "run-2"));
    pump_until([&] { return !results.empty(); });
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.at(0).run_id, "run-2");
    EXPECT_EQ(results.at(0).status, ScriptStatus::Refused);

    release = true;
    pump_until([&] { return results.size() == 2u; });  // run-1's own result

    bus.publish_script_command(command_for("PC-001", "run-2"));
    pump_until([&] { return runner_ptr->body_count() == 2u; });
    EXPECT_EQ(runner_ptr->body_count(), 2u) << "a run that was refused must be retryable";
}

TEST(ClientScripts, ReportsAnErrorRatherThanDyingWhenTheRunnerThrows) {
    // The agent surviving this test at all is half the assertion: an exception
    // out of run() would otherwise take the whole monitoring process down with
    // no log line and no result. Error, not Failed, because nothing ever ran --
    // an exit code here would be a verdict nothing gave.
    MessageBus bus;
    Guarded<ScriptResultMessage> results;
    bus.subscribe_script_result([&](const ScriptResultMessage& r) { results.push(r); });

    ScriptTestHarness harness(bus, "PC-001", true, std::make_unique<ThrowingRunner>());

    bus.publish_script_command(command_for("PC-001", "run-1"));
    pump_until([&] { return !results.empty(); });

    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results.at(0).status, ScriptStatus::Error);
    EXPECT_FALSE(results.at(0).stderr_text.empty()) << "an error must say something";
}

TEST(ClientScripts, KeepsAnnouncingWhileAScriptIsRunning) {
    // The worker thread carries the 10 s announce. If execution sat on it, a
    // long script would push this host past its liveliness lease and the fleet
    // would watch it go Offline mid-run and then come back.
    MessageBus bus;
    std::atomic<bool> release{false};
    // Subscribed before the harness exists so this outlives the worker thread,
    // then cleared below: start()'s own announce would otherwise satisfy the
    // assertion without the worker ever having serviced anything mid-script.
    Guarded<ClientAnnounce> announces;
    bus.subscribe_announce([&](const ClientAnnounce& a) { announces.push(a); });

    auto runner = std::make_unique<RecordingRunner>();
    runner->before_returning = [&] {
        while (!release) {
            std::this_thread::sleep_for(10ms);
        }
    };
    auto* runner_ptr = runner.get();
    ScriptTestHarness harness(bus, "PC-001", true, std::move(runner));
    announces.clear();

    bus.publish_script_command(command_for("PC-001", "run-1"));
    // Asserted, not assumed: without this the announce would arrive just as
    // promptly for a command that was refused, or addressed elsewhere, and the
    // test would be green while proving nothing.
    pump_until([&] { return runner_ptr->body_count() == 1u; });
    ASSERT_EQ(runner_ptr->body_count(), 1u) << "the script never started";

    // While the script is stuck, the worker must still service its own timers.
    harness.force_announce();
    pump_until([&] { return !announces.empty(); });
    release = true;

    EXPECT_FALSE(announces.empty()) << "the monitoring thread was blocked by the script";
}
