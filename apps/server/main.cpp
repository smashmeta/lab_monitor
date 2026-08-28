#include <QApplication>
#include <QDir>
#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QObject>
#include <QStandardPaths>
#include <QString>

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

#include "fleet_window.hpp"
#include "lm/transport/fast_dds_transport.hpp"
#include "lm/transport/in_memory_transport.hpp"
#include "lm/ui/theme.hpp"
#include "server_controller.hpp"

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

    po::options_description description("lab_monitor_server options");
    description.add_options()("help", "print this help message")(
        "domain-id", po::value(&options.domain_id)->default_value(0), "DDS domain id")(
        "config", po::value(&options.config)->default_value(""),
        "path to a server config directory (overrides QStandardPaths::AppConfigLocation)")(
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
        QMessageBox::information(nullptr, QStringLiteral("lab_monitor_server"),
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
    // Required for QStandardPaths::AppConfigLocation and the default
    // QSettings() constructor (used by FleetWindow for geometry/splitter
    // persistence) to resolve to a stable, namespaced location.
    QApplication::setOrganizationName(QStringLiteral("lab_monitor"));
    QApplication::setApplicationName(QStringLiteral("lab_monitor_server"));

    std::optional<Options> parsed;
    try {
        parsed = parse_options(argc, argv);
    } catch (const boost::program_options::error& e) {
        // Logging is not configured yet at this point (it needs
        // options.log_level, which parsing just failed to produce), and this
        // is a WIN32-subsystem binary, so std::cerr has no visible
        // destination either -- a message box is the only way this is ever
        // seen by whoever ran `lab_monitor_server --bogus`.
        Options unused;
        std::ostringstream usage;
        usage << describe_options(unused);
        QMessageBox::critical(nullptr, QStringLiteral("lab_monitor_server"),
                               QStringLiteral("Invalid command line: %1\n\n%2")
                                   .arg(QString::fromStdString(e.what()), QString::fromStdString(usage.str())));
        return EXIT_FAILURE;
    }
    if (!parsed) {
        return EXIT_SUCCESS;
    }
    const Options& options = *parsed;

    // Everything from here on can throw: make_dds_server (four
    // std::runtime_error sites, reachable with e.g. a bad --domain-id or no
    // usable network interface) most notably. Uncaught, that would abort
    // this WIN32-subsystem process with a bare crash dialog and nothing
    // else, despite spdlog being fully configured by the time any of this
    // runs -- log the failure there instead, so it is at least diagnosable
    // from lab_monitor_server.log.
    try {
        const LogTarget log_target = configure_logging("lab_monitor_server", options.log_level);
        log_startup_banner("lab_monitor_server", log_target, options.offline, options.domain_id,
                           options.log_level);

        lm::ui::Theme::apply(app);

        const QString config_dir =
            options.config.empty()
                ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                : QString::fromStdString(options.config);
        QDir().mkpath(config_dir);
        spdlog::info("  config dir  : {}{}", config_dir.toStdString(),
                     options.config.empty() ? " (default)" : " (--config)");

        // Owned by main() for the process lifetime; only referenced when
        // --offline selects the in-memory transport.
        lm::transport::MessageBus offline_bus;

        std::unique_ptr<lm::transport::IServerTransport> transport;
        if (options.offline) {
            transport = lm::transport::make_in_memory_server(offline_bus);
        } else {
            lm::transport::DdsConfig config;
            config.domain_id = options.domain_id;
            transport = lm::transport::make_dds_server(config);
        }
        spdlog::info("  joined      : {}",
                     options.offline ? "in-process bus"
                                     : "DDS domain " + std::to_string(options.domain_id));

        // ServerController lives on the GUI thread for its entire lifetime (no
        // moveToThread, unlike the client's MonitorWorker): the only threading
        // concern here is IServerTransport's callbacks arriving on Fast DDS's
        // own internal threads, which ServerController itself marshals back onto
        // this thread -- see the long comment at the top of server_controller.hpp.
        auto* controller = new ServerController(std::move(transport), config_dir);
        auto* window = new FleetWindow(controller);
        window->set_log_file_path(QString::fromStdString(log_target.path));

        // Context is &app (GUI thread) rather than `controller` or `window`, for
        // the same reason Task 13's client documents at its own aboutToQuit
        // connect: with a context object on a different thread than the sender,
        // the 4-arg connect overload would run the functor on the *context*
        // object's thread instead of the sender's. controller and window both
        // already live on the GUI thread here (neither is ever moveToThread'd),
        // so &app is not strictly required for correctness the way it was for
        // the client's worker-thread case -- but using it keeps the two apps'
        // shutdown paths visibly consistent and removes any doubt.
        //
        // Order matters: controller->stop() resets transport_ synchronously,
        // which runs IServerTransport's destructor (tearing down the Fast DDS
        // participant and joining its internal threads) before returning, so no
        // DDS-thread callback can still be in flight -- or queued and pending --
        // against controller once the deletes below run. Without this, the
        // process would simply exit with controller/window never destroyed at
        // all, leaving DdsServerTransport's destructor (and Fast DDS's clean
        // participant departure) to never run.
        QObject::connect(&app, &QApplication::aboutToQuit, &app, [controller, window] {
            spdlog::info("=== lab_monitor_server shutting down ===");
            controller->stop();
            // After stop(), which tears the participant down synchronously --
            // so this line is the proof the departure was announced rather
            // than the process simply ending.
            spdlog::info("  transport stopped");
            delete window;
            delete controller;
            spdlog::info("  shutdown complete");
        });

        controller->start();

        // Unlike the client's DetailWindow, the fleet console is shown at
        // startup -- it is the whole point of the server application, not an
        // optional detail view behind a tray icon.
        window->show();
        spdlog::info("=== lab_monitor_server running ===");

        return QApplication::exec();
    } catch (const std::exception& e) {
        spdlog::critical("lab_monitor_server: fatal error during startup: {}", e.what());
        return EXIT_FAILURE;
    }
}
