#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QSystemTrayIcon>
#include <QThread>

#include <boost/program_options.hpp>

#include <spdlog/sinks/ringbuffer_sink.h>
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
    bool allow_scripts = false;
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
        "allow-scripts", po::bool_switch(&options.allow_scripts),
        "enrol this machine for remote script execution (off unless asked for)")(
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

/// Where the log ended up, and why, so startup can say both.
struct LogTarget {
    std::string path;
    /// Empty unless the executable's own directory could not be written to.
    std::string fallback_reason;
};

/// Beside the executable, falling back to the per-user data directory.
///
/// The filename used to be relative, which meant the log landed in whatever
/// the process happened to have as its working directory -- the exe's folder
/// when launched from Explorer, a shortcut's "Start in" when launched from
/// one, and C:\Windows\System32 when launched as a service. Nowhere to tell
/// somebody to look. applicationDirPath() pins it.
///
/// That trade has a cost: an install under Program Files is read-only, and a
/// rotating sink that cannot open its file throws. Falling back keeps a
/// locked-down machine -- the kind where the log matters most -- from failing
/// to start over it. The fallback path is reported rather than silently taken,
/// because a log file nobody can find is the problem being fixed here.
LogTarget configure_logging(const std::string& app_name, const std::string& level) {
    constexpr std::size_t kMaxBytes = 5u * 1024u * 1024u;
    constexpr std::size_t kMaxFiles = 3;
    const QString file_name = QString::fromStdString(app_name) + QStringLiteral(".log");

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    // A ring of the most recent lines, so a LogView built later can replay what
    // happened before it existed. configure_logging() necessarily runs before
    // any window does, and the startup banner is the half worth reading.
    sinks.push_back(std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(500));

    LogTarget target;
    const QString beside_exe = QCoreApplication::applicationDirPath() + QChar('/') + file_name;
    try {
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            beside_exe.toStdString(), kMaxBytes, kMaxFiles));
        target.path = beside_exe.toStdString();
    } catch (const spdlog::spdlog_ex& error) {
        const QString data_dir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(data_dir);
        const QString fallback = data_dir + QChar('/') + file_name;
        // Deliberately not guarded: if the per-user data directory is not
        // writable either, there is nowhere left to fall back to, and failing
        // at startup with the reason beats running with no record at all.
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            fallback.toStdString(), kMaxBytes, kMaxFiles));
        target.path = fallback.toStdString();
        target.fallback_reason = std::string("could not write beside the executable (") +
                                 beside_exe.toStdString() + "): " + error.what();
    }

    auto logger = std::make_shared<spdlog::logger>(app_name, sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::from_str(level));
    // info, not warn. These are low-volume, event-driven lines by design --
    // nothing periodic is logged at all -- so the cost of flushing each one is
    // negligible, and the alternative is a file that stays empty while the app
    // runs and loses everything if the process is killed rather than closed.
    // A log you cannot read until the thing you are diagnosing has exited is
    // not much of a log.
    spdlog::flush_on(spdlog::level::info);
    return target;
}

/// The first lines of every run: what this is, how it was told to behave, and
/// where the rest of the file will be. Logged before anything can fail, so a
/// startup that dies still says which build died and with what options.
void log_startup_banner(const std::string& app_name, const LogTarget& target,
                        bool offline, std::uint32_t domain_id, const std::string& level) {
    spdlog::info("=== {} starting ===", app_name);
    spdlog::info("  transport   : {}", offline ? "in-process bus (--offline)" : "DDS");
    spdlog::info("  domain id   : {}", domain_id);
    spdlog::info("  log level   : {}", level);
    spdlog::info("  log file    : {}", target.path);
    if (!target.fallback_reason.empty()) {
        spdlog::warn("  log fallback: {}", target.fallback_reason);
    }
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
        const LogTarget log_target = configure_logging("lab_monitor_client", options.log_level);
        log_startup_banner("lab_monitor_client", log_target, options.offline, options.domain_id,
                           options.log_level);

        lm::ui::Theme::apply(app);

        const std::string host_id = lm::platform::local_host_name();
        spdlog::info("  host id     : {}", host_id);

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
            spdlog::info("  dds probe   : built; DDS rules will be evaluated");
        } else {
            spdlog::info("  dds probe   : skipped (--offline); DDS rules report NotApplicable");
        }
        // Built before the capabilities are settled, because a platform with no
        // runner (Linux, today) must not advertise Scripts and then refuse
        // every command one host at a time -- see make_script_runner().
        std::unique_ptr<lm::platform::IScriptRunner> script_runner =
            lm::platform::make_script_runner();
        const bool scripts_enabled = options.allow_scripts && script_runner != nullptr;
        if (scripts_enabled) {
            capabilities.add(lm::core::Capability::Scripts);
        }
        // Reported whether or not scripts are enabled: it describes this
        // process's token, and the server needs it to say in advance which
        // machines an install would fail on.
        if (lm::platform::is_elevated()) {
            capabilities.add(lm::core::Capability::Elevated);
        }
        spdlog::info("  scripts     : {}",
                     !options.allow_scripts
                         ? "disabled (start with --allow-scripts to enrol)"
                         : (script_runner == nullptr
                                ? "requested, but this platform has no script runner"
                                : (lm::platform::is_elevated()
                                       ? "enabled, elevated"
                                       : "enabled, NOT elevated -- installs "
                                         "and uninstalls will fail")));
        spdlog::info("  capabilities: 0x{:04x}", capabilities.raw());

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
        spdlog::info("  joined      : {}",
                     options.offline ? "in-process bus"
                                     : "DDS domain " + std::to_string(options.domain_id));

        // MonitorWorker owns all probing and messaging and lives entirely on
        // this worker thread; the GUI thread never touches probes_ or
        // transport_ directly, only through the queued connections below.
        auto* worker = new MonitorWorker(std::move(probes), std::move(transport),
                                         std::move(script_runner), scripts_enabled);
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
        window->set_log_file_path(QString::fromStdString(log_target.path));
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
        QObject::connect(tray, &TrayController::quit_requested, &app, [] {
            spdlog::info("quit requested from the tray menu");
            QApplication::quit();
        });
        QObject::connect(window, &DetailWindow::quit_requested, &app, [] {
            // Named separately from the tray route: when a machine stops
            // reporting, the first question is whether somebody meant to stop
            // it, and "which button" is most of that answer.
            spdlog::info("quit requested from the detail window (Close Program)");
            QApplication::quit();
        });

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
            spdlog::info("=== lab_monitor_client shutting down ===");
            worker_thread->quit();
            worker_thread->wait();
            spdlog::info("  worker thread joined");
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
            // After the transport is gone, so this line is the proof a clean
            // DDS departure was announced rather than the process just ending.
            spdlog::info("  shutdown complete");
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
            spdlog::info("  window      : shown (no system tray available)");
        } else {
            spdlog::info("  window      : hidden; the tray icon is the only visible surface");
        }
        spdlog::info("=== lab_monitor_client running ===");

        return QApplication::exec();
    } catch (const std::exception& e) {
        spdlog::critical("lab_monitor_client: fatal error during startup: {}", e.what());
        return EXIT_FAILURE;
    }
}
