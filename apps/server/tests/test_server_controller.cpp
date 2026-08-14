#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

#include <memory>
#include <optional>
#include <algorithm>

#include "lm/core/json.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "server_controller.hpp"

using namespace lm::core;
using namespace lm::transport;

namespace {

/// A bundle with one named template and one assignment, as it would have been
/// written by a previous run of the server.
std::string saved_bundle_json() {
    TemplateBundle bundle;
    bundle.revision = 3;
    bundle.baseline.name = "baseline";

    Rule rule;
    rule.id = "p1";
    rule.description = "Antivirus must be running";
    rule.expectation = Presence::MustBePresent;
    rule.payload = ProcessRule{"antivirus.exe"};

    Template workstation;
    workstation.name = "Lab Workstation";
    workstation.rules.push_back(rule);

    bundle.templates.push_back(workstation);
    bundle.assignments["PC-001"] = {"Lab Workstation"};
    bundle.hash = content_hash(bundle);
    return serialise_bundle(bundle);
}

void write_file(const QString& path, const std::string& text) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text)) << path.toStdString();
    file.write(text.data(), static_cast<qint64>(text.size()));
}

/// Owns the bus so it outlives the controller that references it.
struct Harness {
    MessageBus bus;
    QTemporaryDir dir;
    std::unique_ptr<ServerController> controller;

    Harness() {
        EXPECT_TRUE(dir.isValid());
        controller = std::make_unique<ServerController>(make_in_memory_server(bus), dir.path());
    }
};

}  // namespace

TEST(ServerControllerConfig, LoadsAPersistedBundleOnStart) {
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/bundle.json"), saved_bundle_json());

    harness.controller->start();

    ASSERT_EQ(harness.controller->draft().templates.size(), 1u);
    EXPECT_EQ(harness.controller->draft().templates.front().name, "Lab Workstation");
    EXPECT_EQ(harness.controller->draft().revision, 3u);

    harness.controller->stop();
}

TEST(ServerControllerConfig, AnnouncesTheLoadedBundleSoTheUiCanRenderIt) {
    // The Templates tab is populated in FleetWindow's constructor, which runs
    // BEFORE start() -- so loading the bundle silently leaves the tab showing
    // an empty draft until the operator's first edit happens to trigger a
    // rebuild. Loading must announce itself.
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/bundle.json"), saved_bundle_json());

    QSignalSpy spy(harness.controller.get(), &ServerController::published_changed);
    ASSERT_TRUE(spy.isValid());

    harness.controller->start();

    EXPECT_EQ(spy.count(), 1) << "loading a persisted bundle must emit published_changed";

    harness.controller->stop();
}

TEST(ServerControllerConfig, AnnouncesLoadedExpectedHosts) {
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/expected_hosts.json"),
               R"([{"host_id":"PC-001","address":"10.0.0.1"}])");

    QSignalSpy spy(harness.controller.get(), &ServerController::expected_hosts_changed);
    ASSERT_TRUE(spy.isValid());

    harness.controller->start();

    EXPECT_EQ(harness.controller->expected_hosts().size(), 1u);
    EXPECT_EQ(spy.count(), 1) << "loading expected hosts must emit expected_hosts_changed";

    harness.controller->stop();
}

TEST(ServerControllerConfig, DoesNotAnnounceWhenThereIsNothingToLoad) {
    // A first run has no config files. Emitting anyway would be harmless but
    // misleading, and would make the signal useless as a "content arrived" cue.
    Harness harness;

    QSignalSpy bundle_spy(harness.controller.get(), &ServerController::published_changed);
    QSignalSpy hosts_spy(harness.controller.get(), &ServerController::expected_hosts_changed);

    harness.controller->start();

    EXPECT_EQ(bundle_spy.count(), 0);
    EXPECT_EQ(hosts_spy.count(), 0);

    harness.controller->stop();
}

TEST(ServerControllerStartupAnnounce, PublishesTheLoadedBundleSoClientsGetItWithoutAnEdit) {
    // DDS TRANSIENT_LOCAL durability lives on the DataWriter, not on disk, so a
    // restarted server has written nothing and every client -- including ones
    // that never disconnected -- would sit without a template until the
    // operator happened to make an edit and press Publish.
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/bundle.json"), saved_bundle_json());

    harness.controller->start();

    // Subscribing after start() deliberately mirrors a client joining late: the
    // retained sample must still reach it.
    const auto client = make_in_memory_client(harness.bus);
    std::optional<TemplateBundleMessage> received;
    client->on_bundle([&](const TemplateBundleMessage& message) { received = message; });

    ASSERT_TRUE(received.has_value()) << "no bundle was announced on startup";
    EXPECT_EQ(received->revision, 3u);

    const auto parsed = parse_bundle(received->json);
    ASSERT_TRUE(parsed.has_value()) << parsed.error();
    ASSERT_EQ(parsed->templates.size(), 1u);
    EXPECT_EQ(parsed->templates.front().name, "Lab Workstation");

    harness.controller->stop();
}

TEST(ServerControllerStartupAnnounce, DoesNotBumpTheRevisionOrCreateUnpublishedChanges) {
    // Re-announcing is not re-publishing: bumping on every restart would make
    // every client re-evaluate for nothing and inflate the revision counter.
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/bundle.json"), saved_bundle_json());

    harness.controller->start();

    EXPECT_EQ(harness.controller->published().revision, 3u);
    EXPECT_EQ(harness.controller->draft().revision, 3u);
    EXPECT_FALSE(harness.controller->can_publish())
        << "startup must not leave the Publish button falsely enabled";

    harness.controller->stop();
}

TEST(ServerControllerStartupAnnounce, SaysNothingOnAFirstRunWithNothingPublished) {
    // Nothing has ever been published, so there is no template to distribute.
    Harness harness;

    harness.controller->start();

    const auto client = make_in_memory_client(harness.bus);
    std::optional<TemplateBundleMessage> received;
    client->on_bundle([&](const TemplateBundleMessage& message) { received = message; });

    EXPECT_FALSE(received.has_value()) << "announced a bundle that was never published";

    harness.controller->stop();
}

TEST(ServerControllerExpectedHosts, AHostNamedInATemplateAssignmentCountsAsExpected) {
    // Assigning a template to a machine is a statement that you expect that
    // machine. Requiring it to be typed into a second, separate list as well is
    // a chore that shows up as the host sitting in Unexpected forever.
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/bundle.json"), saved_bundle_json());

    harness.controller->start();

    const std::vector<ExpectedHost> effective = harness.controller->effective_expected_hosts();
    const auto found = std::find_if(effective.begin(), effective.end(), [](const ExpectedHost& host) {
        return host.host_id == "PC-001";
    });
    ASSERT_NE(found, effective.end()) << "a host with a template assignment must be expected";

    harness.controller->stop();
}

TEST(ServerControllerExpectedHosts, AnExplicitEntryKeepsItsAddress) {
    // The explicit list carries an address; the assignment table has no field
    // for one. When a host is in both, the explicit entry must win so the
    // operator's address is not silently dropped.
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/bundle.json"), saved_bundle_json());
    write_file(harness.dir.path() + QStringLiteral("/expected_hosts.json"),
               R"([{"host_id":"PC-001","address":"10.0.0.7"}])");

    harness.controller->start();

    const std::vector<ExpectedHost> effective = harness.controller->effective_expected_hosts();
    const auto found = std::find_if(effective.begin(), effective.end(), [](const ExpectedHost& host) {
        return host.host_id == "PC-001";
    });
    ASSERT_NE(found, effective.end());
    EXPECT_EQ(found->address, "10.0.0.7");
    // And it must appear exactly once, not once per source.
    EXPECT_EQ(std::count_if(effective.begin(), effective.end(),
                            [](const ExpectedHost& h) { return h.host_id == "PC-001"; }),
              1);

    harness.controller->stop();
}

TEST(ServerControllerExpectedHosts, WithNoAssignmentsTheExplicitListIsUsedUnchanged) {
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/expected_hosts.json"),
               R"([{"host_id":"PC-009","address":"10.0.0.9"}])");

    harness.controller->start();

    const std::vector<ExpectedHost> effective = harness.controller->effective_expected_hosts();
    ASSERT_EQ(effective.size(), 1u);
    EXPECT_EQ(effective.front().host_id, "PC-009");
    EXPECT_EQ(effective.front().address, "10.0.0.9");

    harness.controller->stop();
}

TEST(ServerControllerConfig, ACorruptBundleKeepsTheLastGoodStateAndReportsIt) {
    Harness harness;
    write_file(harness.dir.path() + QStringLiteral("/bundle.json"), "{ not json");

    QSignalSpy error_spy(harness.controller.get(), &ServerController::config_error);
    QSignalSpy bundle_spy(harness.controller.get(), &ServerController::published_changed);

    harness.controller->start();

    EXPECT_EQ(error_spy.count(), 1);
    EXPECT_EQ(bundle_spy.count(), 0) << "a failed parse must not announce a bundle that never loaded";
    EXPECT_TRUE(harness.controller->draft().templates.empty());

    harness.controller->stop();
}
