#include "monitor_worker.hpp"

#include <QMetaObject>

#include <spdlog/spdlog.h>

#include <chrono>
#include <expected>
#include <string>
#include <thread>
#include <utility>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"
#include "lm/core/json.hpp"

// qRegisterMetaType<T>() requires a QMetaTypeId<T> specialization, which this
// macro provides. Declared at global scope, as Q_DECLARE_METATYPE requires.
Q_DECLARE_METATYPE(lm::core::ResourceSample)
Q_DECLARE_METATYPE(lm::core::ComplianceReport)
Q_DECLARE_METATYPE(lm::ui::RuleDetail)

namespace {

/// Registration must run before either type is ever carried across a queued
/// connection. Both signal parameters are declared in monitor_worker.hpp with
/// their fully-qualified names (the header lives outside any namespace), so
/// moc records the literal strings "lm::core::ResourceSample" and
/// "lm::core::ComplianceReport" -- the default, no-name qRegisterMetaType<T>()
/// form registers under the same fully-qualified name Q_DECLARE_METATYPE
/// derives, so here the two agree and no explicit name string is required.
/// (Unlike lm_ui's SampleCoalescer, whose signal lives inside a namespace and
/// so needed the explicit-name overload -- see that file for the pitfall.)
void register_metatypes_once() {
    static const bool registered = [] {
        qRegisterMetaType<lm::core::ResourceSample>();
        qRegisterMetaType<lm::core::ComplianceReport>();
        // RuleDetail lives in namespace lm::ui, which is exactly the case that
        // bit SampleCoalescer: moc records whatever literal the header spells,
        // so this is registered under that literal rather than trusting the
        // no-name overload's fully-qualified guess to match.
        qRegisterMetaType<QVector<lm::ui::RuleDetail>>("QVector<lm::ui::RuleDetail>");
        return true;
    }();
    (void)registered;
}

/// This client binary's version, reported in ClientAnnounce. Matches the
/// literal used throughout lm_transport's own tests and the top-level
/// project() version.
constexpr const char* kAgentVersion = "0.1.0";

}  // namespace

MonitorWorker::MonitorWorker(std::unique_ptr<lm::platform::HostProbes> probes,
                              std::unique_ptr<lm::transport::IClientTransport> transport,
                              std::unique_ptr<lm::platform::IScriptRunner> runner,
                              bool allow_scripts, QObject* parent)
    : QObject(parent),
      probes_(std::move(probes)),
      transport_(std::move(transport)),
      runner_(std::move(runner)),
      allow_scripts_(allow_scripts) {
    register_metatypes_once();

    transport_->on_connection_changed(
        [this](lm::transport::ConnectionState state) { emit connection_changed(static_cast<int>(state)); });
}

MonitorWorker::~MonitorWorker() {
    // The run thread posts its result back to this object and reads runner_,
    // both of which are about to stop existing. Joining here is what makes
    // that impossible rather than merely unlikely; a detached thread would
    // leave a use-after-free that only appears when a shutdown lands mid-run.
    // The wait is bounded by the script's own timeout, which the runner
    // enforces.
    if (script_thread_.joinable()) {
        script_thread_.join();
    }
}

void MonitorWorker::announce() {
    lm::transport::ClientAnnounce message;
    message.host_id = probes_->host_id();
    message.agent_version = kAgentVersion;
    message.capabilities = probes_->capabilities().raw();
    // Deliberately not guarded by paused_, unlike the sample and the report.
    // Pausing stops what this machine says about itself; it does not stop the
    // machine saying it is here. The announce is what carries the pause to the
    // server, so silencing it would leave a paused client indistinguishable
    // from a dead one -- which is the whole reading this flag exists to fix.
    message.paused = paused_;
    transport_->publish_announce(message);

    // start() announces at once and the timer every 10 s after, so the
    // seventh announce is the one a minute in. Long enough that a server
    // merely starting up a little later is not accused, short enough that
    // somebody watching a machine come up still sees it.
    constexpr int kAnnouncesBeforeUnheard = 7;
    if (heard_server_ || said_unheard_) {
        return;
    }
    if (++announces_unheard_ >= kAnnouncesBeforeUnheard) {
        said_unheard_ = true;
        emit server_unheard();
    }
}

void MonitorWorker::start() {
    announce();

    // DdsClientTransport::handle_bundle invokes this handler on a Fast DDS
    // listener thread, never on this worker thread. on_bundle() assigns
    // bundle_ (replacing its vectors/maps) and then calls
    // evaluate_compliance(), which the 30 s slow_timer above can also be
    // running concurrently on *this* thread -- collect()/evaluate() hold
    // `const Rule*` pointers into bundle_.templates for the duration of a
    // tick. Without marshalling, a bundle arriving mid-tick would reassign
    // bundle_ out from under those pointers (use-after-free) and would also
    // race paused_ and the probe's CPU-delta state, plus re-enter Fast DDS
    // (publish_report) from inside a reader callback. Posting onto this
    // object's own (worker) thread via a queued connection serialises every
    // bundle application with the timer-driven ticks. Mirrors
    // ServerController::start()'s identical reasoning for its own transport
    // callbacks -- see server_controller.cpp.
    transport_->on_bundle([this](const lm::transport::TemplateBundleMessage& message) {
        QMetaObject::invokeMethod(
            this, [this, message] { on_bundle(message); }, Qt::QueuedConnection);
    });

    // Same marshalling, same reason, as on_bundle above: the transport hands
    // this to us on its own polling/listener thread, and everything the
    // handler touches (executed_, runner_, the transport) belongs to this
    // one. Queued also means the handler returns to that thread immediately
    // rather than holding it for the length of a decision.
    transport_->on_script_command([this](const lm::transport::ScriptCommand& command) {
        QMetaObject::invokeMethod(
            this, [this, command] { on_script_command(command); }, Qt::QueuedConnection);
    });

    // Created here rather than in main(): start() only ever runs after
    // QThread::start(), invoked via a queued connection, so this code is
    // already executing on the worker thread. A QTimer parented to `this`
    // therefore gets the worker thread's affinity for free -- no separate
    // moveToThread dance, and the timeout->slot connections below resolve to
    // ordinary same-thread (direct) calls, never a queued one.
    auto* fast_timer = new QTimer(this);
    fast_timer->setInterval(2000);
    connect(fast_timer, &QTimer::timeout, this, &MonitorWorker::sample_resources);
    fast_timer->start();

    auto* slow_timer = new QTimer(this);
    slow_timer->setInterval(30000);
    connect(slow_timer, &QTimer::timeout, this, &MonitorWorker::evaluate_compliance);
    slow_timer->start();

    // Re-announce on its own slow timer. The announce is TRANSIENT_LOCAL, so a
    // server that starts later still receives the cached one -- but that only
    // covers a server which has never seen this client. It does not cover a
    // server that saw it, lost liveliness (which erases the registry entry,
    // capabilities and all), and then kept receiving resource samples: those
    // recreate the entry with no capabilities, and nothing would ever restore
    // them. Repeating turns that permanent state into a ten-second blip.
    //
    // It also means an upgraded agent's new capabilities reach a running
    // server without anyone restarting anything in the right order.
    auto* announce_timer = new QTimer(this);
    announce_timer->setInterval(10000);
    connect(announce_timer, &QTimer::timeout, this, &MonitorWorker::announce);
    announce_timer->start();
}

void MonitorWorker::sample_resources() {
    const lm::core::ResourceSample sample = probes_->sample_resources();

    if (!paused_) {
        lm::transport::ResourceSampleMessage message;
        message.host_id = probes_->host_id();
        message.sample = sample;
        transport_->publish_resources(message);
    }

    emit resources_sampled(sample);
}

void MonitorWorker::evaluate_compliance() {
    const lm::core::HostFacts facts = probes_->collect(bundle_);
    const lm::core::ComplianceReport report = lm::core::evaluate(bundle_, facts, probes_->capabilities());

    if (!paused_) {
        lm::transport::ComplianceReportMessage message;
        message.report = report;
        transport_->publish_report(message);
    }

    // The report identifies rules by id only, so recover their display fields
    // from the bundle we still hold. rules_for() gives exactly the rules that
    // were evaluated, in the same order.
    QVector<lm::ui::RuleDetail> details;
    for (const lm::core::Rule* rule : lm::core::rules_for(bundle_, probes_->host_id())) {
        // Qualified: ADL searches lm::core (the argument's namespace), not lm::ui.
        details.push_back(lm::ui::describe(*rule));
    }

    emit report_ready(report, details);
}

void MonitorWorker::set_reporting_paused(bool paused) {
    if (paused_ == paused) {
        return;
    }
    paused_ = paused;
    // An operator action, and the explanation for a machine that goes quiet
    // without going away -- worth a line every time it is toggled.
    spdlog::info("reporting {} by the operator", paused ? "paused" : "resumed");
}

void MonitorWorker::on_bundle(const lm::transport::TemplateBundleMessage& message) {
    // Before the revision check, and before parsing: any bundle at all, even a
    // repeat of one already held or one that turns out to be malformed, proves
    // a server found this client and this client found it back. That is the
    // only claim server_unheard() makes.
    heard_server_ = true;

    if (message.revision == bundle_.revision) {
        return;
    }

    const std::expected<lm::core::TemplateBundle, std::string> parsed = lm::core::parse_bundle(message.json);
    if (!parsed) {
        spdlog::error("Discarding template bundle revision {}: {}", message.revision, parsed.error());
        return;  // keep the last good bundle
    }

    bundle_ = *parsed;
    // Guarded by the revision check at the top, so this fires when the server
    // publishes -- not on the 30 s evaluation, and not on the re-announce.
    spdlog::info("applied template bundle revision {} ({} rules apply to this host)",
                 bundle_.revision, lm::core::rules_for(bundle_, probes_->host_id()).size());
    emit template_applied(bundle_.revision);
    evaluate_compliance();
}

void MonitorWorker::refuse_script(const lm::transport::ScriptCommand& command,
                                  const std::string& reason) {
    spdlog::info("refusing script run {} ({}): {}", command.run_id, command.script_name, reason);
    lm::transport::ScriptResultMessage message;
    message.host_id = probes_->host_id();
    message.run_id = command.run_id;
    message.status = lm::core::ScriptStatus::Refused;
    message.refusal_reason = reason;
    transport_->publish_script_result(message);
}

void MonitorWorker::publish_script_outcome(const std::string& run_id,
                                           const std::string& script_name,
                                           const lm::core::ScriptOutcome& outcome) {
    lm::transport::ScriptResultMessage message;
    message.host_id = probes_->host_id();
    message.run_id = run_id;
    // Taken from the outcome, never re-derived: exit code, timeout, never
    // started and the script's own LM-RESULT line all feed one verdict, and a
    // second place weighing them would be free to disagree with the first.
    message.status = outcome.status();
    message.exit_code = outcome.exit_code;
    // "Said nothing" and "said it failed" stay distinguishable on the wire,
    // which is why the flag travels beside the value rather than as a sentinel
    // inside it.
    message.has_reported = outcome.reported.has_value();
    if (outcome.reported) {
        message.reported_ok = outcome.reported->ok;
        message.reported_message = outcome.reported->message;
    }
    message.stdout_text = outcome.stdout_text;
    message.stderr_text = outcome.stderr_text;
    message.duration_ms = outcome.duration_ms;
    transport_->publish_script_result(message);

    spdlog::info("script {} (run {}) {}: exit {} after {} ms", script_name, run_id,
                 lm::core::to_string(message.status), outcome.exit_code, outcome.duration_ms);
}

void MonitorWorker::on_script_command(const lm::transport::ScriptCommand& command) {
    if (command.host_id != probes_->host_id()) {
        return;  // addressed to somebody else
    }

    // Two branches, not one condition, because they answer different questions
    // and only the first is something an operator can act on. Telling somebody
    // on a platform with no runner to pass --allow-scripts sends them to try a
    // flag they have already passed.
    if (!allow_scripts_) {
        // Visibly, not silently. The opt-in is what stops an agent upgrade
        // turning a monitoring box into one that runs remote code, but an
        // operator watching a run that never reports needs to be told why.
        refuse_script(command,
                      "this machine is not enrolled for script execution (--allow-scripts)");
        return;
    }
    if (runner_ == nullptr) {
        refuse_script(command, "this platform has no script runner");
        return;
    }
    if (!executed_.insert(command.run_id).second) {
        // Already done. Silent by design: the server has the first result, and
        // a second one would make the run view contradict itself.
        return;
    }
    if (running_.exchange(true)) {
        executed_.erase(command.run_id);  // it did not run, so let a retry through
        refuse_script(command, "another script is already running on this machine");
        return;
    }

    spdlog::info("running script {} (run {})", command.script_name, command.run_id);

    // On a thread of its own: this one carries the 10 s announce, and a 60 s
    // script blocking it would push the host past its liveliness lease -- the
    // fleet would watch the machine go Offline mid-run and then come back.
    //
    // running_ guarantees at most one live run, so the only thread joined here
    // is a finished one from an earlier command; joining it costs nothing and
    // keeps std::thread's assignment operator from terminating the process.
    if (script_thread_.joinable()) {
        script_thread_.join();
    }
    const std::string body = command.script_body;
    const auto timeout = std::chrono::seconds(command.timeout_seconds);
    const std::string run_id = command.run_id;
    const std::string name = command.script_name;
    script_thread_ = std::thread([this, body, timeout, run_id, name] {
        lm::core::ScriptOutcome outcome;
        try {
            outcome = runner_->run(body, timeout);
        } catch (...) {
            // IScriptRunner promises nothing about throwing, and an escaping
            // exception on a bare std::thread is std::terminate -- a
            // monitoring agent killed with no log line and no result at all.
            // started = false is the outcome's own word for "never ran as a
            // run", so status() reports Error rather than a fabricated exit
            // code. Same reasoning as codec.cpp's decode(): catch the class of
            // risk at the boundary and report it, rather than trusting a
            // dependency to be non-throwing.
            outcome = lm::core::ScriptOutcome{};
            outcome.started = false;
            outcome.stderr_text = "the script runner threw while starting the script";
        }
        // Back onto the worker thread to publish: the transport is this
        // object's, and one thread publishing is one fewer thing the DDS
        // writer has to be safe against.
        QMetaObject::invokeMethod(
            this,
            [this, outcome, run_id, name] {
                publish_script_outcome(run_id, name, outcome);
                running_ = false;
            },
            Qt::QueuedConnection);
    });
}
