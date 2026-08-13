#include <gtest/gtest.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

#include <memory>

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
