#include <gtest/gtest.h>

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
    server->publish_bundle(bundle_message(3, {process_rule("antivirus.exe")}));

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
    server->publish_bundle(bundle_message(1, {process_rule("antivirus.exe")}));
    server->publish_bundle(bundle_message(2, {process_rule("other.exe")}));

    EXPECT_EQ(reports, 2);
}

TEST(MonitorWorker, IgnoresARepublishedIdenticalRevision) {
    Fixture fixture;

    const auto server = make_in_memory_server(fixture.bus);
    int reports = 0;
    server->on_report([&](const ComplianceReportMessage&) { ++reports; });

    fixture.worker->start();
    const TemplateBundleMessage message = bundle_message(1, {process_rule("a.exe")});
    server->publish_bundle(message);
    server->publish_bundle(message);

    EXPECT_EQ(reports, 1);
}

TEST(MonitorWorker, RejectsAMalformedBundleWithoutCrashing) {
    Fixture fixture;
    const auto server = make_in_memory_server(fixture.bus);
    fixture.worker->start();

    TemplateBundleMessage broken;
    broken.revision = 9;
    broken.json = "{ not json";

    EXPECT_NO_THROW(server->publish_bundle(broken));
    EXPECT_EQ(fixture.worker->applied_revision(), 0u);  // last good state retained
}
