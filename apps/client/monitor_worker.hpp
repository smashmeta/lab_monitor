#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include "lm/platform/probes.hpp"
#include "lm/transport/transport.hpp"
#include "lm/ui/rule_detail.hpp"

/// Owns all probing and messaging. Lives on a worker thread; talks to the GUI
/// only through queued signals.
class MonitorWorker : public QObject {
    Q_OBJECT

public:
    /// A null `runner`, or `allow_scripts == false`, both mean the same thing
    /// to a command: refuse it, visibly. The two are kept apart because they
    /// answer different questions -- "this platform cannot" and "this machine
    /// was not enrolled" -- and only the second is something an operator can
    /// change.
    MonitorWorker(std::unique_ptr<lm::platform::HostProbes> probes,
                  std::unique_ptr<lm::transport::IClientTransport> transport,
                  std::unique_ptr<lm::platform::IScriptRunner> runner = nullptr,
                  bool allow_scripts = false, QObject* parent = nullptr);

    /// Joins a script still running. Nothing else can make the detached work
    /// safe: the run thread posts its result back to this object, so letting
    /// it outlive the destructor is a use-after-free that only shows up under
    /// a shutdown timed badly. Bounded by the script's own timeout.
    ~MonitorWorker() override;

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

private slots:
    void on_script_command(const lm::transport::ScriptCommand& command);

private:
    void on_bundle(const lm::transport::TemplateBundleMessage& message);
    /// Fills and publishes the result for one finished run. The verdict is
    /// ScriptOutcome::status()'s alone -- deciding it a second time here from
    /// the exit code would be two sources of truth for one answer.
    void publish_script_outcome(const std::string& run_id, const std::string& script_name,
                                const lm::core::ScriptOutcome& outcome);
    /// Publishes a Refused result. Never silent: an operator whose script did
    /// nothing needs to be told why, and silence is the one response that
    /// cannot be acted on.
    void refuse_script(const lm::transport::ScriptCommand& command, const std::string& reason);

    std::unique_ptr<lm::platform::HostProbes> probes_;
    std::unique_ptr<lm::transport::IClientTransport> transport_;
    std::unique_ptr<lm::platform::IScriptRunner> runner_;
    lm::core::TemplateBundle bundle_;
    bool paused_ = false;
    bool allow_scripts_ = false;
    /// run_ids already executed. Volatile durability prevents replay across a
    /// restart; this prevents a redelivered sample running twice within one.
    std::set<std::string> executed_;
    /// One at a time. A queue would be a promise about ordering and completion
    /// that a machine which may be rebooted cannot keep.
    std::atomic<bool> running_{false};
    /// The current (or last) run. Joined before the next one starts and again
    /// in the destructor, so the thread can never outlive what it posts to.
    std::thread script_thread_;
};
