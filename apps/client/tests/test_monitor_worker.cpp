#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <thread>

#include "lm/core/json.hpp"  // serialise_bundle, content_hash
#include "lm/platform/fakes.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "monitor_worker.hpp"

using namespace lm::core;
using namespace lm::platform;
using namespace lm::transport;

namespace {

struct Fixture {
    MessageBus bus;
    FakeResourceProbe* resources = nullptr;
    FakeProcessProbe* processes = nullptr;
    std::unique_ptr<MonitorWorker> worker;

    Fixture() {
        auto resource_probe = std::make_unique<FakeResourceProbe>();
        auto process_probe = std::make_unique<FakeProcessProbe>();
        resources = resource_probe.get();
        processes = process_probe.get();

        ProbeSet set;
        set.resources = std::move(resource_probe);
        set.processes = std::move(process_probe);

        auto probes = std::make_unique<HostProbes>(
            "PC-001", std::move(set),
            Capabilities{}.add(Capability::Resources).add(Capability::Processes));

        worker = std::make_unique<MonitorWorker>(std::move(probes), make_in_memory_client(bus));
    }
};

TemplateBundleMessage bundle_message(std::uint64_t revision, std::vector<Rule> rules) {
    TemplateBundle bundle;
    bundle.revision = revision;
    Template tmpl;
    tmpl.name = "Lab Workstation";
    tmpl.rules = std::move(rules);
    bundle.templates = {tmpl};
    bundle.assignments["PC-001"] = {"Lab Workstation"};

    TemplateBundleMessage message;
    message.revision = revision;
    message.hash = content_hash(bundle);
    message.json = serialise_bundle(bundle);
    return message;
}

/// Polls until the predicate holds or the timeout expires. Returns whether it
/// held. Mirrors libs/transport/tests/test_fastdds_loopback.cpp's helper of
/// the same shape/purpose.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

/// Fixture's worker runs directly on this test binary's own thread rather
/// than a real QThread (unlike production -- see apps/client/main.cpp),
/// so the on_bundle handler's QMetaObject::invokeMethod(...,
/// Qt::QueuedConnection) -- the C1 fix -- only *posts* an event to that same
/// thread's queue; nothing drains it until QCoreApplication::processEvents()
/// runs. Every test that publishes a bundle and expects it applied before
/// its next assertion needs this pump.
void publish_and_apply(lm::transport::IServerTransport& server, const TemplateBundleMessage& message) {
    server.publish_bundle(message);
    QCoreApplication::processEvents();
}

Rule process_rule(std::string exe) {
    Rule rule;
    rule.id = "p1";
    rule.expectation = Presence::MustBePresent;
    rule.payload = ProcessRule{std::move(exe)};
    return rule;
}

}  // namespace

TEST(MonitorWorker, AnnouncesItselfOnStart) {
    Fixture fixture;

    std::vector<ClientAnnounce> announcements;
    const auto server = make_in_memory_server(fixture.bus);
    server->on_announce([&](const ClientAnnounce& message) { announcements.push_back(message); });

    fixture.worker->start();

    ASSERT_EQ(announcements.size(), 1u);
    EXPECT_EQ(announcements.front().host_id, "PC-001");
    EXPECT_FALSE(announcements.front().agent_version.empty());
}

TEST(MonitorWorker, PublishesResourceSamplesOnTick) {
    Fixture fixture;
    fixture.resources->next.cpu_percent = 44.0;

    std::optional<ResourceSampleMessage> received;
    const auto server = make_in_memory_server(fixture.bus);
    server->on_resources([&](const ResourceSampleMessage& message) { received = message; });

    fixture.worker->sample_resources();

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->host_id, "PC-001");
    EXPECT_DOUBLE_EQ(received->sample.cpu_percent, 44.0);
}

TEST(MonitorWorker, WithNoTemplateReportsResourcesOnly) {
    Fixture fixture;

    std::optional<ComplianceReportMessage> received;
    const auto server = make_in_memory_server(fixture.bus);
    server->on_report([&](const ComplianceReportMessage& message) { received = message; });

    fixture.worker->evaluate_compliance();

    ASSERT_TRUE(received.has_value());
    EXPECT_TRUE(received->report.results.empty());
    EXPECT_EQ(received->report.applied_revision, 0u);
    EXPECT_EQ(fixture.processes->calls, 0);  // nothing to probe
}

TEST(MonitorWorker, AppliesAPublishedTemplateAndReports) {
    Fixture fixture;
    fixture.processes->next = {ProcessInfo{"antivirus.exe", std::nullopt}};

    const auto server = make_in_memory_server(fixture.bus);
    std::optional<ComplianceReportMessage> received;
    server->on_report([&](const ComplianceReportMessage& message) { received = message; });

    fixture.worker->start();
    publish_and_apply(*server, bundle_message(3, {process_rule("antivirus.exe")}));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->report.applied_revision, 3u);
    ASSERT_EQ(received->report.results.size(), 1u);
    EXPECT_EQ(received->report.results.front().status, CheckStatus::Pass);
}

TEST(MonitorWorker, ReevaluatesImmediatelyWhenTheTemplateChanges) {
    Fixture fixture;
    fixture.processes->next = {ProcessInfo{"antivirus.exe", std::nullopt}};

    const auto server = make_in_memory_server(fixture.bus);
    int reports = 0;
    server->on_report([&](const ComplianceReportMessage&) { ++reports; });

    fixture.worker->start();
    publish_and_apply(*server, bundle_message(1, {process_rule("antivirus.exe")}));
    publish_and_apply(*server, bundle_message(2, {process_rule("other.exe")}));

    EXPECT_EQ(reports, 2);
}

TEST(MonitorWorker, IgnoresARepublishedIdenticalRevision) {
    Fixture fixture;

    const auto server = make_in_memory_server(fixture.bus);
    int reports = 0;
    server->on_report([&](const ComplianceReportMessage&) { ++reports; });

    fixture.worker->start();
    const TemplateBundleMessage message = bundle_message(1, {process_rule("a.exe")});
    publish_and_apply(*server, message);
    publish_and_apply(*server, message);

    EXPECT_EQ(reports, 1);
}

// Regression test for the C1 finding: DdsClientTransport::handle_bundle
// invokes the on_bundle handler from a Fast DDS listener thread, never from
// MonitorWorker's own worker thread. MessageBus (used by every other test in
// this file) delivers synchronously on the caller's thread, so it cannot
// exercise that race -- this test instead runs MonitorWorker on a real
// QThread (moveToThread, exactly as apps/client/main.cpp does) and publishes
// the bundle from a genuinely different std::thread, standing in for the DDS
// listener thread. It asserts the handler that actually mutates bundle_ runs
// on the worker thread, by recording std::this_thread::get_id() (genuine OS
// thread identity, not a QThread*, which -- for a thread that is never
// otherwise touched by Qt, such as the raw std::thread below -- can be an
// ad hoc "adopted" QThread wrapper Qt allocates and frees around that
// thread's lifetime, making pointer identity alone an unreliable signal
// once that thread has exited) from inside a directly-connected slot on
// template_applied (emitted from on_bundle itself, after bundle_ has been
// reassigned), and comparing it against worker_thread's own OS thread id
// (captured the same way, via a blocking call executed on worker_thread).
// Against the unfixed code (a bare lambda registered with on_bundle), this
// fails: on_bundle runs on the publishing std::thread, not worker_thread,
// which is exactly the unsynchronised-mutation-of-bundle_ bug this test
// exists to catch.
TEST(MonitorWorker, AppliesBundlesOnItsOwnWorkerThreadNotTheCallersThread) {
    lm::transport::MessageBus bus;

    auto resource_probe = std::make_unique<FakeResourceProbe>();
    auto process_probe = std::make_unique<FakeProcessProbe>();
    ProbeSet set;
    set.resources = std::move(resource_probe);
    set.processes = std::move(process_probe);
    auto probes = std::make_unique<HostProbes>(
        "PC-001", std::move(set), Capabilities{}.add(Capability::Resources).add(Capability::Processes));

    // Heap-allocated (not a unique_ptr in this scope) because ownership moves
    // to worker_thread via moveToThread; it is deleted explicitly below once
    // worker_thread has been joined, mirroring the I3 fix's reasoning for why
    // deleteLater is unsafe once the owning thread's event loop has stopped.
    auto* worker = new MonitorWorker(std::move(probes), make_in_memory_client(bus));

    QThread worker_thread;
    worker->moveToThread(&worker_thread);
    worker_thread.start();

    // Blocks until start() has actually run on worker_thread, so on_bundle's
    // handler is registered with the transport before this test publishes.
    QMetaObject::invokeMethod(worker, "start", Qt::BlockingQueuedConnection);

    // Ground truth for worker_thread's real OS thread id, obtained the same
    // way (a blocking call executed on that thread), not from QThread's own
    // bookkeeping, so the comparison below never depends on any Qt-internal
    // pointer identity.
    std::thread::id worker_os_id;
    QMetaObject::invokeMethod(
        worker, [&worker_os_id] { worker_os_id = std::this_thread::get_id(); }, Qt::BlockingQueuedConnection);

    // Qt::DirectConnection is explicit and load-bearing here: it forces this
    // lambda to run synchronously, on whatever thread actually performs the
    // emit -- exactly the thing under test. (The default Qt::AutoConnection
    // would decide Direct vs Queued by comparing that same emitting thread
    // against *this connection's* receiving thread, i.e. worker_thread,
    // which would silently launder the bug this test exists to catch.)
    std::atomic<bool> handled{false};
    std::thread::id observed_os_id;
    QObject::connect(
        worker, &MonitorWorker::template_applied, worker,
        [&] {
            observed_os_id = std::this_thread::get_id();
            handled = true;
        },
        Qt::DirectConnection);

    const auto server = make_in_memory_server(bus);
    const std::thread::id main_os_id = std::this_thread::get_id();

    // Published from a thread that is neither this test's thread nor
    // worker_thread -- the same shape as a Fast DDS listener thread calling
    // into DdsClientTransport::handle_bundle in production.
    std::thread publisher([&] { server->publish_bundle(bundle_message(7, {process_rule("a.exe")})); });
    const std::thread::id publisher_os_id = publisher.get_id();
    publisher.join();

    // Poll rather than assume synchronous delivery: under the fix, on_bundle
    // runs asynchronously, whenever worker_thread's own event loop gets
    // around to draining the queued call -- not necessarily by the time
    // publisher.join() returns.
    ASSERT_TRUE(wait_for([&] { return handled.load(); })) << "template_applied was never emitted";
    EXPECT_EQ(observed_os_id, worker_os_id);
    EXPECT_NE(observed_os_id, main_os_id);
    EXPECT_NE(observed_os_id, publisher_os_id);

    worker_thread.quit();
    worker_thread.wait();
    delete worker;
}

TEST(MonitorWorker, RejectsAMalformedBundleWithoutCrashing) {
    Fixture fixture;
    const auto server = make_in_memory_server(fixture.bus);
    fixture.worker->start();

    TemplateBundleMessage broken;
    broken.revision = 9;
    broken.json = "{ not json";

    EXPECT_NO_THROW(publish_and_apply(*server, broken));
    EXPECT_EQ(fixture.worker->applied_revision(), 0u);  // last good state retained
}
