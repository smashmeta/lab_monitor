#include <QApplication>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QThread>

#include <boost/program_options.hpp>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "detail_window.hpp"
#include "lm/core/types.hpp"
#include "lm/platform/probes.hpp"
#include "lm/transport/fast_dds_transport.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "lm/ui/theme.hpp"
#include "monitor_worker.hpp"
#include "tray_controller.hpp"

namespace {

struct Options {
    int domain_id = 0;
    std::string config;
    bool offline = false;
    std::string log_level = "info";
};

/// Returns nullopt only for --help, which has already printed its own output.
std::optional<Options> parse_options(int argc, char** argv) {
    namespace po = boost::program_options;

    Options options;
    po::options_description description("lab_monitor_client options");
    description.add_options()("help", "print this help message")(
        "domain-id", po::value(&options.domain_id)->default_value(0), "DDS domain id")(
        "config", po::value(&options.config)->default_value(""),
        "path to a client config file (reserved for future use)")(
        "offline", po::bool_switch(&options.offline),
        "use an in-process transport instead of a real DDS domain")(
        "log-level", po::value(&options.log_level)->default_value("info"),
        "trace|debug|info|warn|err|critical|off");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, description), vm);
    po::notify(vm);

    if (vm.count("help") != 0u) {
        std::cout << description << '\n';
        return std::nullopt;
    }
    return options;
}

void configure_logging(const std::string& level) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "lab_monitor_client.log", 5u * 1024u * 1024u, 3));

    auto logger = std::make_shared<spdlog::logger>("lab_monitor_client", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::from_str(level));
    spdlog::flush_on(spdlog::level::warn);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // A tray app must not exit just because its (hidden-by-default) window
    // closes; the tray icon's "Quit" action is the only way out.
    app.setQuitOnLastWindowClosed(false);

    const std::optional<Options> parsed = parse_options(argc, argv);
    if (!parsed) {
        return EXIT_SUCCESS;
    }
    const Options& options = *parsed;

    configure_logging(options.log_level);
    spdlog::info("lab_monitor_client starting (offline={}, domain-id={})", options.offline, options.domain_id);

    lm::ui::Theme::apply(app);

    const std::string host_id = lm::platform::local_host_name();

    // Owned by main() for the process lifetime; only referenced when
    // --offline selects the in-memory transport.
    lm::transport::MessageBus offline_bus;

    auto probes = std::make_unique<lm::platform::HostProbes>(
        host_id, lm::platform::make_platform_probes(), lm::core::platform_capabilities());

    std::unique_ptr<lm::transport::IClientTransport> transport;
    if (options.offline) {
        transport = lm::transport::make_in_memory_client(offline_bus);
    } else {
        lm::transport::DdsConfig config;
        config.domain_id = options.domain_id;
        transport = lm::transport::make_dds_client(config);
    }

    // MonitorWorker owns all probing and messaging and lives entirely on
    // this worker thread; the GUI thread never touches probes_ or
    // transport_ directly, only through the queued connections below.
    auto* worker = new MonitorWorker(std::move(probes), std::move(transport));
    auto* worker_thread = new QThread();
    worker->moveToThread(worker_thread);

    const QString qt_host_id = QString::fromStdString(host_id);
    auto* window = new DetailWindow(qt_host_id);
    auto* tray = new TrayController(qt_host_id, window);

    // Cross-thread connections resolve to Qt::QueuedConnection by default
    // (Qt::AutoConnection), which is exactly what is required here -- do
    // not force Qt::DirectConnection on any of these.
    QObject::connect(worker, &MonitorWorker::resources_sampled, window, &DetailWindow::apply_resources);
    QObject::connect(worker, &MonitorWorker::resources_sampled, tray, &TrayController::apply_resources);
    QObject::connect(worker, &MonitorWorker::report_ready, window, &DetailWindow::apply_report);
    QObject::connect(worker, &MonitorWorker::report_ready, tray, &TrayController::apply_report);
    QObject::connect(worker, &MonitorWorker::template_applied, window, &DetailWindow::set_applied_revision);
    QObject::connect(worker, &MonitorWorker::template_applied, tray, &TrayController::set_applied_revision);
    QObject::connect(worker, &MonitorWorker::connection_changed, window, &DetailWindow::set_connected);
    QObject::connect(worker, &MonitorWorker::connection_changed, tray, &TrayController::set_connected);

    QObject::connect(tray, &TrayController::reporting_paused_changed, worker,
                      &MonitorWorker::set_reporting_paused);
    QObject::connect(tray, &TrayController::quit_requested, &app, &QApplication::quit);

    // Context must be &app (GUI thread), never `worker`: the 4-arg connect
    // overload runs the functor on the *context* object's thread when
    // sender and context live on different threads. With `worker` as
    // context, worker_thread->wait() would execute on the worker thread
    // itself -- Qt detects that self-wait, logs "QThread::wait: Thread
    // tried to wait on itself", and returns immediately without actually
    // waiting, so the worker thread is never joined and may still be
    // mid-shutdown (or mid-publish, on a live DDS transport) when the
    // process exits.
    QObject::connect(&app, &QApplication::aboutToQuit, &app, [worker, worker_thread] {
        worker_thread->quit();
        worker_thread->wait();
        worker->deleteLater();
    });
    QObject::connect(worker_thread, &QThread::finished, worker_thread, &QThread::deleteLater);

    worker_thread->start();
    QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);

    // The window is deliberately never shown here: the app starts hidden,
    // with only the tray icon visible, until the user opens it.
    return QApplication::exec();
}
