#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "lm/core/script.hpp"
#include "lm/core/types.hpp"
#include "lm/platform/fakes.hpp"
#include "lm/platform/probes.hpp"
#include "lm/transport/fast_dds_transport.hpp"
#include "monitor_worker.hpp"

using namespace lm::transport;
using namespace std::chrono_literals;

TEST(FastDdsScripts, ARealCommandRunsARealScriptAndTheResultComesBack) {
    // Everything except the two GUIs: a real DDS domain, the real worker, the
    // real PowerShell runner, a real script. Every other test in this feature
    // substitutes at least one of those, so this is the only one that would
    // notice them disagreeing.
    //
    // It lives here rather than beside the transport's own loopback tests
    // because the client half of it is MonitorWorker: this target already
    // compiles that source and already links Qt, lm_ui and spdlog, where
    // libs/transport would have had to acquire all three to hold one test.
    int argc = 1;
    char program[] = "lab_monitor_client_dds_tests";
    std::array<char*, 2> argv{program, nullptr};
    // The worker's thread runs a Qt event loop, and every hop into it is a
    // queued invocation -- neither works without an application object.
    QCoreApplication app(argc, argv.data());

    // Declared before `server` and so, by reverse-declaration-order
    // destruction, outliving it: both are captured by reference in a handler
    // Fast DDS runs on its own listener thread. The publish loop below stops
    // at the *first* arrival while duplicates already in flight can still be
    // delivered afterwards -- including during the server's own destruction --
    // so neither the vector nor the mutex guarding it may be destroyed first.
    // Same reasoning, and same shape, as FastDdsLoopback's own script test.
    std::mutex results_mutex;
    std::vector<ScriptResultMessage> results;

    DdsConfig config;
    // Its own domain, so a fleet running on 42 while this test does cannot
    // answer for the machine under test.
    config.domain_id = 72;
    const auto server = make_dds_server(config);

    server->on_script_result([&](const ScriptResultMessage& result) {
        const std::lock_guard<std::mutex> lock(results_mutex);
        results.push_back(result);
    });
    const auto result_count = [&] {
        const std::lock_guard<std::mutex> lock(results_mutex);
        return results.size();
    };

    lm::platform::ProbeSet probes;
    probes.resources = std::make_unique<lm::platform::FakeResourceProbe>();
    auto host_probes = std::make_unique<lm::platform::HostProbes>(
        "PC-integration", std::move(probes), lm::core::platform_capabilities());

    auto* worker = new MonitorWorker(std::move(host_probes), make_dds_client(config),
                                     lm::platform::make_script_runner(),
                                     /*allow_scripts=*/true);
    auto* thread = new QThread();
    worker->moveToThread(thread);
    thread->start();
    QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);

    ScriptCommand command;
    command.host_id = "PC-integration";
    command.run_id = "run-e2e";
    command.script_name = "(custom script)";
    command.script_body = "Write-Output 'ran end to end'\nexit 0\n";
    command.timeout_seconds = 30;

    // Published in a loop because discovery races a single publish. Republishing
    // is safe: the client remembers the run ids it has executed, so the script
    // runs exactly once however many samples arrive.
    for (int attempt = 0; attempt < 60 && result_count() == 0; ++attempt) {
        server->publish_script_command(command);
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(250ms);
    }

    thread->quit();
    thread->wait();
    delete worker;
    delete thread;

    std::vector<ScriptResultMessage> received;
    {
        const std::lock_guard<std::mutex> lock(results_mutex);
        received = results;
    }
    ASSERT_FALSE(received.empty()) << "no result came back";
    EXPECT_EQ(received.front().status, lm::core::ScriptStatus::Completed);
    EXPECT_NE(received.front().stdout_text.find("ran end to end"), std::string::npos)
        << received.front().stdout_text;
}
