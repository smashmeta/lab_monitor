#include "server_controller.hpp"

#include <QByteArray>
#include <QHash>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QMetaObject>

#include <algorithm>
#include <exception>
#include <expected>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "lm/core/json.hpp"
#include "lm/ui/rule_detail.hpp"

ServerController::ServerController(std::unique_ptr<lm::transport::IServerTransport> transport,
                                    QString config_dir, QObject* parent)
    : QObject(parent), transport_(std::move(transport)), config_dir_(std::move(config_dir)) {}

bool ServerController::can_publish() const {
    return lm::core::content_hash(draft_) != lm::core::content_hash(published_);
}

void ServerController::stop() {
    reconcile_timer_.stop();
    // Resetting transport_ runs IServerTransport's destructor synchronously,
    // right here, on the GUI thread. For the DDS transport that tears down
    // the Fast DDS participant (delete_contained_entities() +
    // delete_participant()), which announces a clean departure to other
    // participants and joins Fast DDS's own internal threads before this
    // call returns -- so by the time stop() returns, no DDS thread can still
    // be mid-callback or about to post a new QMetaObject::invokeMethod
    // against this object. That is what makes it safe for the caller to
    // delete this controller immediately afterward.
    transport_.reset();
}

void ServerController::start() {
    load_config();

    // Every IServerTransport callback below may be invoked from a Fast DDS
    // internal thread (or, for --offline, synchronously from whichever
    // thread calls publish_*() on the in-memory bus -- still not necessarily
    // the GUI thread). registry_ and model_ are only ever touched from the
    // functors that QMetaObject::invokeMethod posts onto *this* object's
    // thread, never directly inside these lambdas.
    transport_->on_announce([this](const lm::transport::ClientAnnounce& announce) {
        QMetaObject::invokeMethod(
            this, [this, announce] { on_announce(announce); }, Qt::QueuedConnection);
    });
    transport_->on_resources([this](const lm::transport::ResourceSampleMessage& sample) {
        // The one exception: SampleCoalescer::push() is documented and
        // implemented as thread-safe (an internal std::mutex protects
        // pending_), so it is safe to call directly from whatever thread
        // this callback runs on. It does not touch registry_ or model_
        // itself -- that happens later in apply_coalesced(), reached only
        // through SampleCoalescer's own QTimer, which lives on the thread
        // the coalescer was constructed on (this object's GUI thread).
        coalescer_.push(sample);
    });
    transport_->on_report([this](const lm::transport::ComplianceReportMessage& report) {
        QMetaObject::invokeMethod(
            this, [this, report] { on_report(report); }, Qt::QueuedConnection);
    });
    transport_->on_client_lost([this](const lm::core::HostId& host_id) {
        QMetaObject::invokeMethod(
            this, [this, host_id] { on_client_lost(host_id); }, Qt::QueuedConnection);
    });

    connect(&coalescer_, &lm::ui::SampleCoalescer::flushed, this, &ServerController::apply_coalesced);

    reconcile_timer_.setInterval(1000);
    connect(&reconcile_timer_, &QTimer::timeout, this, &ServerController::reconcile_now);
    reconcile_timer_.start();

    announce_published();

    reconcile_now();
}

std::vector<lm::core::ExpectedHost> ServerController::effective_expected_hosts() const {
    std::vector<lm::core::ExpectedHost> hosts = expected_;

    // Explicit entries win, so an address typed into the expected-host list is
    // never overwritten by an assignment-derived entry that has none.
    for (const auto& [host_id, template_names] : published_.assignments) {
        const bool already_listed =
            std::ranges::any_of(hosts, [&](const lm::core::ExpectedHost& host) {
                return host.host_id == host_id;
            });
        if (!already_listed) {
            hosts.push_back(lm::core::ExpectedHost{host_id, std::string{}});
        }
    }

    return hosts;
}

void ServerController::announce_published() {
    // DDS TRANSIENT_LOCAL durability belongs to the DataWriter, not to disk, so
    // a freshly started server has written nothing: without this, clients sit
    // without a template until the operator happens to make an edit and press
    // Publish. Re-announcing the bundle we already have is deliberately NOT a
    // publish -- the revision and hash stay exactly as they were saved, so a
    // client that already applied this revision short-circuits and re-evaluates
    // nothing, and restarts cannot inflate the revision counter.
    if (published_.revision == 0) {
        return;  // nothing has ever been published; there is no template to distribute
    }

    lm::transport::TemplateBundleMessage message;
    message.revision = published_.revision;
    message.hash = published_.hash;
    message.json = lm::core::serialise_bundle(published_);
    transport_->publish_bundle(message);

    spdlog::info("announced template bundle revision {} on startup", published_.revision);
}

void ServerController::mark_draft_dirty() { emit draft_publishable_changed(can_publish()); }

void ServerController::set_expected_hosts(std::vector<lm::core::ExpectedHost> hosts) {
    expected_ = std::move(hosts);
    save_expected_hosts();
    emit expected_hosts_changed();
    reconcile_now();
}

void ServerController::add_expected_host(const lm::core::HostId& host_id, const std::string& address) {
    const auto it = std::ranges::find(expected_, host_id, &lm::core::ExpectedHost::host_id);
    if (it != expected_.end()) {
        it->address = address;
    } else {
        expected_.push_back(lm::core::ExpectedHost{host_id, address});
    }
    save_expected_hosts();
    emit expected_hosts_changed();
    reconcile_now();
}

void ServerController::remove_expected_host(const lm::core::HostId& host_id) {
    // std::erase_if returns how many it removed, so the erase-remove pair and
    // its "did anything change?" iterator comparison both collapse into this.
    if (std::erase_if(expected_, [&](const lm::core::ExpectedHost& host) {
            return host.host_id == host_id;
        }) == 0) {
        return;
    }
    save_expected_hosts();
    emit expected_hosts_changed();
    reconcile_now();
}

void ServerController::publish() {
    if (!can_publish()) {
        return;
    }

    draft_.revision = published_.revision + 1;
    draft_.hash = lm::core::content_hash(draft_);
    published_ = draft_;

    save_published_bundle();

    lm::transport::TemplateBundleMessage message;
    message.revision = published_.revision;
    message.hash = published_.hash;
    message.json = lm::core::serialise_bundle(published_);
    transport_->publish_bundle(message);

    options_.current_revision = published_.revision;

    emit published_changed();
    emit draft_publishable_changed(can_publish());
    // Refresh staleness against the new revision immediately rather than
    // waiting up to 1 s for the next timer tick.
    reconcile_now();
}

void ServerController::on_announce(const lm::transport::ClientAnnounce& announce) {
    registry_.record_announce(announce.host_id, lm::core::Capabilities(announce.capabilities),
                               announce.paused, lm::core::Clock::now());
    // Hearing from a machine is the evidence that moves it out of Missing, so
    // reflect it now rather than up to a second later on the timer. Announces
    // arrive every 10 s per client, so this costs nothing.
    reconcile_now();
}

QVector<lm::ui::ComplianceTag> ServerController::failing_tags(
    const lm::core::ComplianceReport& report) const {
    QHash<QString, QString> labels;
    for (const lm::core::Rule* rule : lm::core::rules_for(published_, report.host_id)) {
        labels.insert(QString::fromStdString(rule->id), lm::ui::describe(*rule).label);
    }

    QVector<lm::ui::ComplianceTag> tags;
    for (const lm::core::CheckResult& result : report.results) {
        // Fail and Error only. A pass is counted rather than listed -- a wall of
        // green pushes the two red lines that matter off the row -- and a rule
        // the host cannot evaluate is not a problem with the host.
        if (result.status != lm::core::CheckStatus::Fail &&
            result.status != lm::core::CheckStatus::Error) {
            continue;
        }
        const QString id = QString::fromStdString(result.rule_id);
        const auto label = labels.constFind(id);
        // The id only when the rule is gone, which means the bundle changed
        // between the client evaluating and this arriving. Better a join key on
        // screen than a blank tag with nothing to look up.
        tags.push_back({label != labels.constEnd() ? *label : id, result.status});
    }

    // Failures before errors, and otherwise the order the client reported them
    // in. The cell shows as many tags as fit and then "+3 more", so what gets
    // truncated is decided here -- and a rule that is definitely broken must
    // not be pushed off the row by one that merely could not be read.
    std::stable_sort(tags.begin(), tags.end(),
                     [](const lm::ui::ComplianceTag& lhs, const lm::ui::ComplianceTag& rhs) {
                         return lhs.status == lm::core::CheckStatus::Fail &&
                                rhs.status != lm::core::CheckStatus::Fail;
                     });
    return tags;
}

void ServerController::on_report(const lm::transport::ComplianceReportMessage& report) {
    registry_.record_report(report.report.host_id, report.report.applied_revision, lm::core::Clock::now());
    // Before the signal below, not after: the Compliance tab reads liveness and
    // the report together, and a host whose first report has just arrived would
    // otherwise still be drawn as silent.
    reconcile_now();

    const QString host_id = QString::fromStdString(report.report.host_id);
    report_cache_.insert(host_id, report.report);
    // The fleet row is coloured by compliance as well as liveness, and
    // core::FleetEntry carries only the latter. The row also names the rules
    // this host is not passing, and a CheckResult carries only a rule id -- so
    // the descriptions are recovered here, where the published bundle is, and
    // handed to the model rather than the model learning to look them up.
    model_.apply_compliance(report.report, failing_tags(report.report));
    emit compliance_report_received(host_id, report.report);
}

void ServerController::on_client_lost(const lm::core::HostId& host_id) {
    // Expected hosts are deliberately left in the registry with their last
    // last_seen timestamp: reconcile()'s own liveliness-lease check (run
    // every 1 s by reconcile_timer_) independently transitions them from
    // Online to Offline once `now - last_seen` exceeds the lease -- which is
    // exactly the Offline (not Missing) behaviour the vertical slice expects
    // when a client stops. Erasing here would instead make them Missing the
    // instant this callback fires.
    //
    // Unexpected hosts get no such lease check at all (see
    // lm::core::reconcile's unexpected-entries loop, which never calls
    // within_lease()), so without removing them here they would linger in
    // the fleet view forever after disconnecting. mark_lost() is reserved
    // for exactly that case.
    const bool is_expected =
        std::ranges::any_of(expected_, [&](const lm::core::ExpectedHost& host) {
            return host.host_id == host_id;
        });
    if (!is_expected) {
        registry_.mark_lost(host_id);
    }
    reconcile_now();
}

void ServerController::apply_coalesced(QVector<lm::transport::ResourceSampleMessage> batch) {
    const lm::core::TimePoint now = lm::core::Clock::now();
    for (const lm::transport::ResourceSampleMessage& sample : batch) {
        registry_.record_sample(sample.host_id, now);
        model_.apply_sample(sample);

        const QString host_id = QString::fromStdString(sample.host_id);
        resource_cache_.insert(host_id, sample.sample);
        emit resource_sample_received(host_id, sample.sample);
    }
}

void ServerController::reconcile_now() {
    options_.current_revision = published_.revision;
    lm::core::FleetView view = lm::core::reconcile(
        effective_expected_hosts(), registry_.snapshot(), lm::core::Clock::now(), options_);
    model_.apply(view);

    // Only the host -> state mapping, not the whole view: last_seen moves on
    // every sample, so comparing entries outright would report a change on
    // every tick of the 1 s timer. reconcile() returns them in a deterministic
    // order, so equal states really do mean an unchanged fleet.
    const bool states_changed = !std::ranges::equal(
        fleet_.entries, view.entries, [](const lm::core::FleetEntry& lhs, const lm::core::FleetEntry& rhs) {
            return lhs.host_id == rhs.host_id && lhs.state == rhs.state;
        });

    fleet_ = std::move(view);
    emit counts_changed(fleet_.counts);
    if (states_changed) {
        emit fleet_changed();
    }
}

QString ServerController::expected_hosts_path() const {
    return config_dir_ + QStringLiteral("/expected_hosts.json");
}

QString ServerController::bundle_path() const { return config_dir_ + QStringLiteral("/bundle.json"); }

void ServerController::load_config() {
    QDir().mkpath(config_dir_);

    QFile expected_file(expected_hosts_path());
    if (expected_file.exists() && expected_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray raw = expected_file.readAll();
        try {
            const nlohmann::json doc = nlohmann::json::parse(raw.constData(), raw.constData() + raw.size());
            std::vector<lm::core::ExpectedHost> hosts;
            for (const auto& item : doc) {
                lm::core::ExpectedHost host;
                host.host_id = item.at("host_id").get<std::string>();
                host.address = item.value("address", std::string{});
                hosts.push_back(std::move(host));
            }
            expected_ = std::move(hosts);
            // load_config() runs inside start(), which the caller invokes AFTER
            // constructing the window -- so the window has already populated
            // itself from an empty controller. Announce, or it stays empty
            // until some unrelated edit happens to trigger a rebuild.
            emit expected_hosts_changed();
        } catch (const std::exception& error) {
            const QString message =
                QStringLiteral("Failed to parse expected hosts config: %1").arg(error.what());
            spdlog::error(message.toStdString());
            emit config_error(message);
            // Keep whatever expected_ already held (empty on a first run).
        }
    }

    QFile bundle_file(bundle_path());
    if (bundle_file.exists() && bundle_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QByteArray raw = bundle_file.readAll();
        const std::expected<lm::core::TemplateBundle, std::string> parsed =
            lm::core::parse_bundle(std::string(raw.constData(), static_cast<std::size_t>(raw.size())));
        if (parsed) {
            published_ = *parsed;
            draft_ = published_;
            options_.current_revision = published_.revision;

            // Bundles written while ids were typed by hand can hold the same id
            // twice, which costs a rule: rules_for() keeps the first and drops
            // the rest. Repair the draft only -- published_ is the record of
            // what clients actually hold, and rewriting it would misreport the
            // fleet. The operator publishes the repair when they choose to.
            for (const auto& [before, after] : lm::core::deduplicate_rule_ids(draft_)) {
                spdlog::warn("duplicate rule id '{}' in the loaded bundle; renamed to '{}' in the "
                              "draft -- publish to apply it to the fleet",
                              before, after);
            }
            // Same reason as expected_hosts_changed() above: the Templates tab
            // was built before this ran and holds an empty draft.
            emit published_changed();
        } else {
            const QString message = QStringLiteral("Failed to parse template bundle config: %1")
                                         .arg(QString::fromStdString(parsed.error()));
            spdlog::error(message.toStdString());
            emit config_error(message);
            // Keep the last good bundle in memory -- default-constructed
            // (empty) on a first run, or whatever was already loaded.
        }
    }
}

void ServerController::save_expected_hosts() {
    nlohmann::json doc = nlohmann::json::array();
    for (const lm::core::ExpectedHost& host : expected_) {
        doc.push_back({{"host_id", host.host_id}, {"address", host.address}});
    }

    const QString path = expected_hosts_path();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        const QString message =
            QStringLiteral("Failed to save expected hosts config to %1: %2").arg(path, file.errorString());
        spdlog::error(message.toStdString());
        emit config_error(message);
        return;
    }

    const std::string text = doc.dump(2);
    const qint64 written = file.write(text.data(), static_cast<qint64>(text.size()));
    if (written != static_cast<qint64>(text.size())) {
        const QString message =
            QStringLiteral("Failed to write expected hosts config to %1: %2").arg(path, file.errorString());
        spdlog::error(message.toStdString());
        emit config_error(message);
    }
}

void ServerController::save_published_bundle() {
    const QString path = bundle_path();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        const QString message =
            QStringLiteral("Failed to save template bundle config to %1: %2").arg(path, file.errorString());
        spdlog::error(message.toStdString());
        emit config_error(message);
        return;
    }

    const std::string text = lm::core::serialise_bundle(published_);
    const qint64 written = file.write(text.data(), static_cast<qint64>(text.size()));
    if (written != static_cast<qint64>(text.size())) {
        const QString message =
            QStringLiteral("Failed to write template bundle config to %1: %2").arg(path, file.errorString());
        spdlog::error(message.toStdString());
        emit config_error(message);
    }
}
