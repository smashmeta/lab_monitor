#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <memory>
#include <string>
#include <vector>

#include "lm/core/client_registry.hpp"
#include "lm/core/compliance.hpp"
#include "lm/core/fleet.hpp"
#include "lm/core/host_facts.hpp"
#include "lm/core/template_bundle.hpp"
#include "lm/transport/messages.hpp"
#include "lm/transport/transport.hpp"
#include "lm/ui/fleet_model.hpp"
#include "lm/ui/sample_coalescer.hpp"

/// Owns the IServerTransport, the ClientRegistry, the expected-host list, and
/// both the published and draft template bundles.
///
/// Lives entirely on the GUI thread and is never moved off it. Fast DDS
/// invokes IServerTransport's callbacks on its own internal threads; every
/// one of them is marshalled onto this object's thread with
/// QMetaObject::invokeMethod(this, <functor>, Qt::QueuedConnection) *before*
/// it touches registry_ or model_ -- ClientRegistry is deliberately not
/// thread-safe, so that hop is mandatory. The one exception is resource
/// samples, which go straight to SampleCoalescer::push() (documented and
/// implemented as thread-safe via an internal mutex) rather than through
/// invokeMethod; the registry/model update for those happens later, in
/// apply_coalesced(), which SampleCoalescer only ever calls from its own
/// thread (this object's GUI thread) via its internal QTimer. See start().
class ServerController : public QObject {
    Q_OBJECT

public:
    ServerController(std::unique_ptr<lm::transport::IServerTransport> transport, QString config_dir,
                      QObject* parent = nullptr);

    /// Loads persisted config, wires transport callbacks and starts the 1 s
    /// reconcile timer. Call once, on the GUI thread, after construction.
    void start();

    /// Deterministic shutdown: stops the reconcile timer and resets
    /// transport_, which runs IServerTransport's destructor synchronously
    /// (for the DDS transport, this tears down the Fast DDS participant --
    /// announcing a clean departure and joining its internal threads --
    /// before this call returns). Call once, on the GUI thread, before
    /// destroying this controller, so no DDS-thread callback can ever be
    /// mid-flight (or queued and pending) against an object that is about to
    /// be deleted.
    void stop();

    /// Owned by this controller; FleetWindow wraps it in a
    /// QSortFilterProxyModel rather than taking ownership.
    [[nodiscard]] lm::ui::FleetModel* model() { return &model_; }

    [[nodiscard]] const std::vector<lm::core::ExpectedHost>& expected_hosts() const { return expected_; }

    /// The explicit expected-host list unioned with every host named in a
    /// template assignment, which is what reconcile() is actually given.
    ///
    /// Assigning a template to a machine is already a statement that you expect
    /// that machine; making the operator also type it into a separate list just
    /// leaves the host sitting in Unexpected. Explicit entries win, so an
    /// address typed there is never dropped.
    [[nodiscard]] std::vector<lm::core::ExpectedHost> effective_expected_hosts() const;
    [[nodiscard]] const lm::core::TemplateBundle& published() const { return published_; }
    /// Mutable: the Templates tab edits rules/assignments directly through
    /// this reference, then calls mark_draft_dirty() to refresh
    /// can_publish()/draft_publishable_changed().
    [[nodiscard]] lm::core::TemplateBundle& draft() { return draft_; }
    [[nodiscard]] bool can_publish() const;

    [[nodiscard]] const QMap<QString, lm::core::ResourceSample>& resource_cache() const {
        return resource_cache_;
    }
    [[nodiscard]] const QMap<QString, lm::core::ComplianceReport>& report_cache() const {
        return report_cache_;
    }

    /// The most recent reconciliation. FleetModel holds the same information
    /// but only exposes it a cell at a time through data(); a view that needs
    /// to reason about liveness — the Compliance tab, which has to say that a
    /// host is silent rather than leave it out — wants the entries themselves.
    [[nodiscard]] const lm::core::FleetView& fleet() const { return fleet_; }

public slots:
    /// Recomputes can_publish() and emits draft_publishable_changed(). Call
    /// after mutating draft() from the outside.
    void mark_draft_dirty();

    void set_expected_hosts(std::vector<lm::core::ExpectedHost> hosts);
    /// Adds host_id to the expected list (or updates its address if already
    /// present), persists, and re-reconciles immediately.
    void add_expected_host(const lm::core::HostId& host_id, const std::string& address);
    void remove_expected_host(const lm::core::HostId& host_id);

    /// Bumps revision, recomputes content_hash, persists, and calls
    /// transport_->publish_bundle(). No-op when can_publish() is false.
    void publish();

signals:
    void counts_changed(lm::core::FleetCounts counts);
    /// A host appeared, departed, or changed state. Deliberately not emitted on
    /// every reconcile: last_seen advances on each sample, so a view that
    /// compared whole entries would fire once a second and rebuild anything
    /// listening to it just as often.
    void fleet_changed();
    void expected_hosts_changed();
    void resource_sample_received(QString host_id, lm::core::ResourceSample sample);
    void compliance_report_received(QString host_id, lm::core::ComplianceReport report);
    void draft_publishable_changed(bool can_publish);
    void published_changed();
    /// A config file existed but failed to parse. The message is meant for
    /// direct display (e.g. a status bar or message box); the last good
    /// in-memory bundle/expected-host list is left untouched.
    void config_error(QString message);

private:
    void on_announce(const lm::transport::ClientAnnounce& announce);
    void on_report(const lm::transport::ComplianceReportMessage& report);
    void on_client_lost(const lm::core::HostId& host_id);
    void apply_coalesced(QVector<lm::transport::ResourceSampleMessage> batch);
    void reconcile_now();

    void load_config();

    /// Re-writes the already-published bundle to the transport at its existing
    /// revision, so clients receive it after a server restart without waiting
    /// for an edit. Not a publish: nothing is bumped, hashed or saved.
    /// Every Fail and Error in a report, labelled with its rule's description.
    ///
    /// Through core::rules_for(), never a walk over every template: this host's
    /// rules are the baseline plus the templates assigned to it, which is
    /// exactly the set the client evaluated. Walking all of them instead lets a
    /// rule id reused in an unrelated template win the lookup and label a row
    /// with a different rule's description -- and resolves collisions the
    /// opposite way round from rules_for(), so the two ends disagree about the
    /// same id.
    [[nodiscard]] QVector<lm::ui::ComplianceTag> failing_tags(
        const lm::core::ComplianceReport& report) const;

    void announce_published();
    /// Emits config_error() on a failed open or a short/failed write,
    /// mirroring load_config()'s own failure handling. No longer const,
    /// since a failure path now emits a signal.
    void save_expected_hosts();
    void save_published_bundle();
    [[nodiscard]] QString expected_hosts_path() const;
    [[nodiscard]] QString bundle_path() const;

    std::unique_ptr<lm::transport::IServerTransport> transport_;
    QString config_dir_;

    lm::core::ClientRegistry registry_;
    lm::ui::FleetModel model_;
    lm::ui::SampleCoalescer coalescer_;
    QTimer reconcile_timer_;

    lm::core::FleetView fleet_;
    std::vector<lm::core::ExpectedHost> expected_;
    lm::core::TemplateBundle draft_;
    lm::core::TemplateBundle published_;
    lm::core::ReconcileOptions options_;

    QMap<QString, lm::core::ResourceSample> resource_cache_;
    QMap<QString, lm::core::ComplianceReport> report_cache_;
};
