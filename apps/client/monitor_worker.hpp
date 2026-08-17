#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include <memory>

#include "lm/platform/probes.hpp"
#include "lm/transport/transport.hpp"
#include "lm/ui/rule_detail.hpp"

/// Owns all probing and messaging. Lives on a worker thread; talks to the GUI
/// only through queued signals.
class MonitorWorker : public QObject {
    Q_OBJECT

public:
    MonitorWorker(std::unique_ptr<lm::platform::HostProbes> probes,
                  std::unique_ptr<lm::transport::IClientTransport> transport,
                  QObject* parent = nullptr);

    [[nodiscard]] std::uint64_t applied_revision() const { return bundle_.revision; }

public slots:
    /// Announces this client and subscribes to template updates.
    void start();
    /// Re-publishes the announce. Called on a timer as well as at start,
    /// because the announce is the only carrier of this client's capabilities
    /// and the server can lose them: ClientRegistry::mark_lost() erases the
    /// whole entry on a liveliness drop, and the resource samples that follow
    /// recreate it with no capabilities at all. Announcing once meant those
    /// never came back, and the server showed the machine as unable to report
    /// adapters for the rest of its run.
    void announce();
    /// Fast tick: samples resources and publishes them.
    void sample_resources();
    /// Slow tick: collects facts, evaluates the template, publishes the report.
    void evaluate_compliance();
    void set_reporting_paused(bool paused);

signals:
    void resources_sampled(lm::core::ResourceSample sample);
    /// Carries the display fields for every rule alongside the report, because
    /// core::CheckResult itself only ever carries a rule id. The worker holds
    /// the parsed bundle, so it is the one place that can recover them without
    /// widening the wire format.
    void report_ready(lm::core::ComplianceReport report, QVector<lm::ui::RuleDetail> details);
    void template_applied(quint64 revision);
    void connection_changed(int state);

private:
    void on_bundle(const lm::transport::TemplateBundleMessage& message);

    std::unique_ptr<lm::platform::HostProbes> probes_;
    std::unique_ptr<lm::transport::IClientTransport> transport_;
    lm::core::TemplateBundle bundle_;
    bool paused_ = false;
};
