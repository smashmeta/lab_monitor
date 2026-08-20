#include <QApplication>
#include <QMessageBox>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>
#include <QThread>

#include <boost/program_options.hpp>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "detail_window.hpp"
#include "lm/core/types.hpp"
#include "lm/platform/probes.hpp"
#include "lm/transport/dds_probe.hpp"
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

/// Binds description's option values to `options` without parsing anything,
/// so both parse_options() and main()'s boost::program_options::error
/// handler (which has no parsed Options to work with, and needs the
/// description purely to render usage text) can build the same description.
boost::program_options::options_description describe_options(Options& options) {
    namespace po = boost::program_options;

    po::options_description description("lab_monitor_client options");
    description.add_options()("help", "print this help message")(
        "domain-id", po::value(&options.domain_id)->default_value(0), "DDS domain id")(
        "config", po::value(&options.config)->default_value(""),
        "path to a client config file (reserved for future use)")(
        "offline", po::bool_switch(&options.offline),
        "use an in-process transport instead of a real DDS domain")(
        "log-level", po::value(&options.log_level)->default_value("info"),
        "trace|debug|info|warn|err|critical|off");
    return description;
}

/// Returns nullopt only for --help, which has already shown its own message
/// box. Throws boost::program_options::error (e.g. unknown_option,
/// invalid_option_value) for anything unparseable -- the caller must catch
/// it, since this WIN32-subsystem process has no visible stderr for an
/// uncaught exception to be reported through before logging is configured.
std::optional<Options> parse_options(int argc, char** argv) {
    namespace po = boost::program_options;

    Options options;
    po::options_description description = describe_options(options);

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, description), vm);
    po::notify(vm);

    if (vm.count("help") != 0u) {
        std::ostringstream usage;
        usage << description;
        QMessageBox::information(nullptr, QStringLiteral("lab_monitor_client"),
                                  QString::fromStdString(usage.str()));
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

    std::optional<Options> parsed;
    try {
        parsed = parse_options(argc, argv);
    } catch (const boost::program_options::error& e) {
        // Logging is not configured yet at this point (it needs
        // options.log_level, which parsing just failed to produce), and this
        // is a WIN32-subsystem binary, so std::cerr has no visible
        // destination either -- a message box is the only way this is ever
        // seen by whoever ran `lab_monitor_client --bogus`.
        Options unused;
        std::ostringstream usage;
        usage << describe_options(unused);
        QMessageBox::critical(nullptr, QStringLiteral("lab_monitor_client"),
                               QStringLiteral("Invalid command line: %1\n\n%2")
                                   .arg(QString::fromStdString(e.what()), QString::fromStdString(usage.str())));
        return EXIT_FAILURE;
    }
    if (!parsed) {
        return EXIT_SUCCESS;
    }
    const Options& options = *parsed;

    // Everything from here on can throw: make_dds_client (four
    // std::runtime_error sites, reachable with e.g. a bad --domain-id or no
    // usable network interface) most notably. Uncaught, that would abort
    // this WIN32-subsystem process with a bare crash dialog and nothing
    // else, despite spdlog being fully configured by the time any of this
    // runs -- log the failure there instead, so it is at least diagnosable
    // from lab_monitor_client.log.
    try {
        configure_logging(options.log_level);
        spdlog::info("lab_monitor_client starting (offline={}, domain-id={})", options.offline, options.domain_id);

        lm::ui::Theme::apply(app);

        const std::string host_id = lm::platform::local_host_name();

        // Owned by main() for the process lifetime; only referenced when
        // --offline selects the in-memory transport.
        lm::transport::MessageBus offline_bus;

        lm::platform::ProbeSet probe_set = lm::platform::make_platform_probes();
        // The DDS probe is assembled here rather than inside
        // make_platform_probes() because it is the one probe that is not about
        // this platform at all: it reads a bus the *monitored application*
        // uses, its implementation lives in lm_transport, and --offline means
        // "do not touch DDS", which has to be honoured for it too. Leaving it
        // null drops Capability::Dds, and every DDS rule then reports
        // NotApplicable rather than failing.
        lm::core::Capabilities capabilities = lm::core::platform_capabilities();
        if (!options.offline) {
            probe_set.dds = lm::transport::make_dds_probe();
            capabilities.add(lm::core::Capability::Dds);
        }

        auto probes = std::make_unique<lm::platform::HostProbes>(host_id, std::move(probe_set),
                                                                 capabilities);

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

        // A desktop without a working tray (e.g. GNOME without an
        // appindicator extension, a bare WM, a container) would otherwise
        // leave this app with zero UI surface and no in-app way to quit:
        // setQuitOnLastWindowClosed(false) below plus a window that starts
        // hidden and, per DetailWindow::closeEvent, only ever hides on close
        // -- never actually quits -- normally relies entirely on the tray's
        // "Quit" action to ever end the process.
        const bool tray_available = QSystemTrayIcon::isSystemTrayAvailable();
        if (!tray_available) {
            spdlog::warn(
                "System tray unavailable; showing the main window at startup and enabling "
                "quit-on-last-window-closed so the app remains closable");
        }
        // A tray app must not exit just because its (hidden-by-default)
        // window closes; the tray icon's "Quit" action is the only way out
        // -- unless there is no usable tray, in which case the window is
        // the only surface this process has, and must behave like a normal
        // closable window instead.
        app.setQuitOnLastWindowClosed(!tray_available);

        const QString qt_host_id = QString::fromStdString(host_id);
        auto* window = new DetailWindow(qt_host_id);
        window->set_tray_available(tray_available);
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
        // Both quit routes -- the tray's Quit action and the window's confirmed
        // Close Program button -- land on the same shutdown, so the worker
        // teardown below runs identically whichever one the user reached for.
        QObject::connect(tray, &TrayController::quit_requested, &app, &QApplication::quit);
        QObject::connect(window, &DetailWindow::quit_requested, &app, &QApplication::quit);

        // Context must be &app (GUI thread), never `worker`: the 4-arg connect
        // overload runs the functor on the *context* object's thread when
        // sender and context live on different threads. With `worker` as
        // context, worker_thread->wait() would execute on the worker thread
        // itself -- Qt detects that self-wait, logs "QThread::wait: Thread
        // tried to wait on itself", and returns immediately without actually
        // waiting, so the worker thread is never joined and may still be
        // mid-shutdown (or mid-publish, on a live DDS transport) when the
        // process exits.
        QObject::connect(&app, &QApplication::aboutToQuit, &app, [worker, worker_thread, window, tray] {
            worker_thread->quit();
            worker_thread->wait();
            // worker's thread affinity is worker_thread, which has just been
            // joined and is therefore no longer running an event loop --
            // deleteLater() would post a DeferredDelete event to a queue
            // nobody will ever drain, so ~MonitorWorker (and with it
            // DdsClientTransport's destructor, which announces a clean DDS
            // departure) would simply never run, and the server would wait
            // out the full liveliness lease instead of unmatching
            // immediately. Delete synchronously instead: nothing can still
            // be touching worker once its thread has been joined.
            delete worker;
            // window and tray are both parentless `new`s (TrayController's
            // constructor takes `window` as a plain DetailWindow* argument,
            // not a QObject parent -- see tray_controller.hpp), so nothing
            // else ever deletes them either, leaving the tray icon
            // uncleaned. tray is deleted first since it holds a raw pointer
            // back to window.
            delete tray;
            delete window;
        });
        QObject::connect(worker_thread, &QThread::finished, worker_thread, &QThread::deleteLater);

        worker_thread->start();
        QMetaObject::invokeMethod(worker, "start", Qt::QueuedConnection);

        // When a tray is available, the window is deliberately never shown
        // here: the app starts hidden, with only the tray icon visible,
        // until the user opens it. Without one, the window is this app's
        // only UI surface, so it must be visible from the start.
        if (!tray_available) {
            window->show();
        }

        return QApplication::exec();
    } catch (const std::exception& e) {
        spdlog::critical("lab_monitor_client: fatal error during startup: {}", e.what());
        return EXIT_FAILURE;
    }
}
