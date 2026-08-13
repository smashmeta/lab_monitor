#include "monitor_worker.hpp"

#include <QMetaObject>

#include <spdlog/spdlog.h>

#include <expected>
#include <utility>

#include "lm/core/compliance.hpp"
#include "lm/core/host_facts.hpp"
#include "lm/core/json.hpp"

// qRegisterMetaType<T>() requires a QMetaTypeId<T> specialization, which this
// macro provides. Declared at global scope, as Q_DECLARE_METATYPE requires.
Q_DECLARE_METATYPE(lm::core::ResourceSample)
Q_DECLARE_METATYPE(lm::core::ComplianceReport)
Q_DECLARE_METATYPE(RuleDetail)

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
        // RuleDetail is declared at global scope, so moc records the signal
        // parameter as the literal "QVector<RuleDetail>" -- which is what the
        // default qRegisterMetaType<T>() form registers here too, so the two
        // agree without an explicit name string.
        qRegisterMetaType<QVector<RuleDetail>>();
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
                              QObject* parent)
    : QObject(parent), probes_(std::move(probes)), transport_(std::move(transport)) {
    register_metatypes_once();

    transport_->on_connection_changed(
        [this](lm::transport::ConnectionState state) { emit connection_changed(static_cast<int>(state)); });
}

void MonitorWorker::start() {
    lm::transport::ClientAnnounce announce;
    announce.host_id = probes_->host_id();
    announce.agent_version = kAgentVersion;
    announce.capabilities = probes_->capabilities().raw();
    transport_->publish_announce(announce);

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
    QVector<RuleDetail> details;
    for (const lm::core::Rule* rule : lm::core::rules_for(bundle_, probes_->host_id())) {
        details.push_back(describe(*rule));
    }

    emit report_ready(report, details);
}

void MonitorWorker::set_reporting_paused(bool paused) { paused_ = paused; }

void MonitorWorker::on_bundle(const lm::transport::TemplateBundleMessage& message) {
    if (message.revision == bundle_.revision) {
        return;
    }

    const std::expected<lm::core::TemplateBundle, std::string> parsed = lm::core::parse_bundle(message.json);
    if (!parsed) {
        spdlog::error("Discarding template bundle revision {}: {}", message.revision, parsed.error());
        return;  // keep the last good bundle
    }

    bundle_ = *parsed;
    emit template_applied(bundle_.revision);
    evaluate_compliance();
}
