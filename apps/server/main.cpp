#include <QApplication>
#include <QDir>
#include <QObject>
#include <QStandardPaths>
#include <QString>

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

/// Returns nullopt only for --help, which has already printed its own output.
std::optional<Options> parse_options(int argc, char** argv) {
    namespace po = boost::program_options;

    Options options;
    po::options_description description("lab_monitor_server options");
    description.add_options()("help", "print this help message")(
        "domain-id", po::value(&options.domain_id)->default_value(0), "DDS domain id")(
        "config", po::value(&options.config)->default_value(""),
        "path to a server config directory (overrides QStandardPaths::AppConfigLocation)")(
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
        "lab_monitor_server.log", 5u * 1024u * 1024u, 3));

    auto logger = std::make_shared<spdlog::logger>("lab_monitor_server", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::from_str(level));
    spdlog::flush_on(spdlog::level::warn);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Required for QStandardPaths::AppConfigLocation and the default
    // QSettings() constructor (used by FleetWindow for geometry/splitter
    // persistence) to resolve to a stable, namespaced location.
    QApplication::setOrganizationName(QStringLiteral("lab_monitor"));
    QApplication::setApplicationName(QStringLiteral("lab_monitor_server"));

    const std::optional<Options> parsed = parse_options(argc, argv);
    if (!parsed) {
        return EXIT_SUCCESS;
    }
    const Options& options = *parsed;

    configure_logging(options.log_level);
    spdlog::info("lab_monitor_server starting (offline={}, domain-id={})", options.offline, options.domain_id);

    lm::ui::Theme::apply(app);

    const QString config_dir =
        options.config.empty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            : QString::fromStdString(options.config);
    QDir().mkpath(config_dir);

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

    // ServerController lives on the GUI thread for its entire lifetime (no
    // moveToThread, unlike the client's MonitorWorker): the only threading
    // concern here is IServerTransport's callbacks arriving on Fast DDS's
    // own internal threads, which ServerController itself marshals back onto
    // this thread -- see the long comment at the top of server_controller.hpp.
    auto* controller = new ServerController(std::move(transport), config_dir);
    auto* window = new FleetWindow(controller);

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
        controller->stop();
        delete window;
        delete controller;
    });

    controller->start();

    // Unlike the client's DetailWindow, the fleet console is shown at
    // startup -- it is the whole point of the server application, not an
    // optional detail view behind a tray icon.
    window->show();

    return QApplication::exec();
}
